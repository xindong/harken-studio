// Session-trace deterministic replay, end to end: a scripted session is
// recorded in-process on the null backend while its render is captured
// block by block; `harken_host trace-replay` then re-executes the journal
// twice as a subprocess. Acceptance bars: the two replay captures are
// byte-identical to each other AND to the original session's render (the
// engine-determinism claim made observable), and a content file whose hash
// no longer matches the journal refuses the replay with a report naming
// the file — never a silently different render.
//
// The scripted session deliberately interleaves API calls with pending
// starts and Vn_Update barriers: a canonical nested Action starts alongside
// direct Voices, and a fade-stop crosses the middle of the pump loop.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>
#include <harken/BusTopologyBuilder.hpp>

#include "Common/RecipePackTestUtil.h"
#include "Common/TestUtil.h"
#include "TraceCodec.h"
#include "ActionAdmission.h"
#include "ProjectGenerationTransaction.h"
#include "TraceFormat.h"
#include "TraceRecorder.h"
#include "Engine.h"
#include "harken/core/CurrentWorldArtifact.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/core/MediaContentRevision.h"
#include "harken/media/CurrentWorldBodyVintage.h"
#include "harken/media/PackFormat.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/param/MeterProbe.h"
#include "harken/param/VariableService.h"
#include "harken/render/BuiltinEffects.h"
#include "harken_preview/PreviewAbi.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <process.h>
#include <windows.h>

#ifndef HARKEN_HOST_PATH
#error "HARKEN_HOST_PATH must point at the harken_host executable"
#endif
#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

using harkentest::EngineSession;
using harkentest::MakeAttenuationDesc;
using harkentest::MakeNullConfig;
using harkentest::MonoDesc;

namespace {

constexpr std::uint32_t kBlockFrames = 256;

harken::PlayablePropertyPayloadViewV1 EmptyOccurrencePayload() {
    harken::PlayablePropertyPayloadViewV1 payload;
    payload.values[3].bits = 1;
    payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    return payload;
}

harken::PlayablePropertyPayloadViewV1 ResolvedDefinitionPayload(
    VnBusId route = VN_BUS_MASTER) {
    auto payload = EmptyOccurrencePayload();
    payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    payload.values[6].objectId = route;
    return payload;
}

std::string TempPath(const std::string& name) {
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    return std::string(HARKEN_TEMP_DIR "/") + "trace_replay_" +
           std::to_string(GetCurrentProcessId()) + "_" + name;
}

struct HostRun {
    int exitCode = -1;
    std::string output;
};

HostRun RunHost(const std::string& args);
HostRun RunMutatedTrace(const std::vector<std::uint8_t>& bytes,
                        const std::string& suffix);
std::vector<std::uint8_t> ReadFileBytes(const std::string& path);
void CaptureLastBlock(harken::Engine* engine,
                      std::vector<std::uint8_t>* bytes);
float PeakOfRawF32(const std::vector<std::uint8_t>& bytes);

#if HARKEN_DEVELOPMENT_PREVIEW
TEST(TraceReplayE2E,
     CurrentWorldQueueOriginPcmAndTypedPreviewReplayDeterministically) {
    constexpr VnObjectId kSound = harkentest::ObjectId(0x9501);
    constexpr VnObjectId kContainer = harkentest::ObjectId(0x9502);
    const std::string tracePath = TempPath("preview_playable_vtrc4.vtrc");
    const std::string out1 = TempPath("preview_playable_vtrc4_1.f32");
    const std::string out2 = TempPath("preview_playable_vtrc4_2.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(out1);
    std::filesystem::remove(out2);
    std::vector<float> samples(4096, 0.25f);
    const float* channels[1] = {samples.data()};
    std::uint64_t contentRevision = 0;
    ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
        48000, 1, samples.size(), channels, 1, &contentRevision));

    std::vector<std::uint8_t> original;
    {
        EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const auto canonicalPayload = ResolvedDefinitionPayload();
        std::array<harken::PlayablePropertyProgramViewV1, 2> properties{};
        properties[0].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Sound;
        properties[0].stableId = harkentest::ObjectId(kSound);
        properties[0].contentRevision = contentRevision;
        properties[0].routeStableId = VN_BUS_MASTER;
        properties[0].compatibilityFingerprint = 1;
        properties[0].payload = canonicalPayload;
        properties[0].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[0]);
        properties[1].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        properties[1].authoredStableOrder = 1;
        properties[1].stableId = harkentest::ObjectId(kContainer);
        properties[1].contentRevision = 1;
        properties[1].routeStableId = VN_BUS_MASTER;
        properties[1].compatibilityFingerprint = 2;
        properties[1].payload = canonicalPayload;
        properties[1].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[1]);
        harken::ContainerChildProgramViewV2 child{};
        child.playable.kind = harken::PlayableKindV1::Sound;
        child.playable.stableId = harkentest::ObjectId(kSound);
        child.playable.expectedProgramSemanticFingerprint =
            properties[0].programSemanticFingerprint;
        child.playable.contentRevision = contentRevision;
        child.playable.routeStableId = VN_BUS_MASTER;
        child.playable.compatibilityFingerprint = 1;
        child.stableChildOrdinal = 7;
        harken::AuthoredCompletionLeafV1 completionLeaf{};
        completionLeaf.stableLeafOrdinal = 1;
        completionLeaf.content = child.playable;
        completionLeaf.startOffset = {
            harken::AuthoredTimeDomainV1::SourceFrames, 0, 1};
        completionLeaf.duration = {
            harken::AuthoredTimeDomainV1::NaturalMediaEnd, 0, 1};
        harkentest::CopyLeafTimingToHint(child, completionLeaf);
        harken::PlayableOccurrencePropertyViewV1 occurrence{};
        occurrence.parentKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        occurrence.stableOccurrenceOrdinal = child.stableChildOrdinal;
        occurrence.parentStableId = harkentest::ObjectId(kContainer);
        occurrence.parentProgramSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        occurrence.childPlayable = child.playable;
        occurrence.payload = EmptyOccurrencePayload();
        occurrence.occurrenceSemanticFingerprint =
            harken::ComputePlayableOccurrenceFingerprintV1(occurrence);
        harken::ContainerProgramViewV3 container{};
        container.containerStableId = harkentest::ObjectId(kContainer);
        container.programSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        container.mode = harken::ContainerModeV1::SequenceQueue;
        container.memoryScope = harken::MemoryScopeV1::Emitter;
        container.childCount = 1;
        container.children = &child;
        harken::ContainerWorldInstallViewV1 world{};
        world.playableProperties = properties.data();
        world.playablePropertyCount = properties.size();
        world.playableOccurrences = &occurrence;
        world.playableOccurrenceCount = 1;
        world.containers = &container;
        world.containerCount = 1;
        world.identity.capabilityMask =
            harken::kWorldCapabilityPlayableProperties |
            harken::kWorldCapabilitySequenceQueue;
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
        ASSERT_EQ(harken::InstallContainerWorldV1(*engine, world).status,
                  harken::WorldInstallStatus::Ok);
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &pcm));

        VnPreviewPlayPlayableDescV2 desc{};
        desc.structSize = sizeof(desc);
        desc.playableKind = VN_PREVIEW_PLAYABLE_KIND_CONTAINER;
        desc.stableId = harkentest::ObjectId(kContainer);
        VnPreviewPlayableHandle handle = 0;
        ASSERT_EQ(VN_PREVIEW_OK,
                  VnPreview_PlayPlayable(&desc, &handle));
        VnPreviewPlayableStateV2 state{};
        state.structSize = sizeof(state);
        ASSERT_EQ(VN_PREVIEW_OK,
                  VnPreview_QueryPlayable(handle, &state));
        EXPECT_EQ(0u, state.lastAppliedBlockSequence);
        for (std::uint32_t block = 0; block < 8; ++block) {
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
            ASSERT_EQ(VN_OK, Vn_Update());
            state = {};
            state.structSize = sizeof(state);
            ASSERT_EQ(VN_PREVIEW_OK,
                      VnPreview_QueryPlayable(handle, &state));
        }
        ASSERT_EQ(VN_PREVIEW_OK, VnPreview_StopPlayable(handle, 0));
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);
    const HostRun run1 =
        RunHost("trace-replay " + tracePath + " --capture " + out1);
    ASSERT_EQ(0, run1.exitCode) << run1.output;
    const HostRun run2 =
        RunHost("trace-replay " + tracePath + " --capture " + out2);
    ASSERT_EQ(0, run2.exitCode) << run2.output;
    const auto replay1 = ReadFileBytes(out1);
    const auto replay2 = ReadFileBytes(out2);
    ASSERT_FALSE(replay1.empty());
    EXPECT_EQ(replay1, replay2);
    EXPECT_EQ(original, replay1);
}

TEST(TraceReplayE2E,
     VariableControlDualGridOriginRetargetQueryStopReplaysExactly) {
    constexpr VnSoundId kSoundA = harkentest::ObjectId(0x9511);
    constexpr VnSoundId kSoundB = harkentest::ObjectId(0x9512);
    constexpr VnObjectId kContainer = harkentest::ObjectId(0x9513);
    constexpr VnVariableId kDiscrete = harkentest::ObjectId(0x9514);
    constexpr VnVariableId kContinuous = harkentest::ObjectId(0x9515);
    const std::string tracePath = TempPath("variable_dual_vtrc4.vtrc");
    const std::string out1 = TempPath("variable_dual_vtrc4_1.f32");
    const std::string out2 = TempPath("variable_dual_vtrc4_2.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(out1);
    std::filesystem::remove(out2);

    std::vector<float> samplesA(8192, 0.25f);
    std::vector<float> samplesB(8192, -0.125f);
    const float* channelsA[1] = {samplesA.data()};
    const float* channelsB[1] = {samplesB.data()};
    std::uint64_t revisionA = 0;
    std::uint64_t revisionB = 0;
    ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
        48000, 1, samplesA.size(), channelsA, 1, &revisionA));
    ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
        48000, 1, samplesB.size(), channelsB, 1, &revisionB));

    std::vector<std::uint8_t> original;
    {
        EngineSession session(MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const auto payload = ResolvedDefinitionPayload();
        std::array<harken::VariableDefinitionV1, 2> variables{{
            {kDiscrete, harken::VariableKindV1::Step, 0.0f, 8.0f, 3.0f},
            {kContinuous, harken::VariableKindV1::Smooth,
             0.0f, 8.0f, 2.0f},
        }};
        std::array<harken::PlayablePropertyProgramViewV1, 3> properties{};
        const std::array<VnObjectId, 2> soundIds{kSoundA, kSoundB};
        const std::array<std::uint64_t, 2> revisions{revisionA, revisionB};
        for (std::uint32_t i = 0; i < 2; ++i) {
            properties[i].contentPropertyKind =
                harken::playableproperties::ContentPropertyKindV1::Sound;
            properties[i].authoredStableOrder = i;
            properties[i].stableId = harkentest::ObjectId(soundIds[i]);
            properties[i].contentRevision = revisions[i];
            properties[i].routeStableId = VN_BUS_MASTER;
            properties[i].compatibilityFingerprint = i + 1u;
            properties[i].payload = payload;
            properties[i].programSemanticFingerprint =
                harken::ComputePlayablePropertyFingerprintV1(properties[i]);
        }
        properties[2].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        properties[2].authoredStableOrder = 2;
        properties[2].stableId = harkentest::ObjectId(kContainer);
        properties[2].contentRevision = 1;
        properties[2].routeStableId = VN_BUS_MASTER;
        properties[2].compatibilityFingerprint = 3;
        properties[2].payload = payload;
        properties[2].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[2]);

        std::array<harken::ContainerChildProgramViewV2, 4> children{};
        std::array<harken::PlayableOccurrencePropertyViewV1, 4> occurrences{};
        for (std::uint32_t i = 0; i < children.size(); ++i) {
            const std::uint32_t soundIndex = i & 1u;
            children[i].stableChildOrdinal = i + 1u;
            children[i].authoredStableOrder = i;
            children[i].playable.kind = harken::PlayableKindV1::Sound;
            children[i].playable.stableId = harkentest::ObjectId(soundIds[soundIndex]);
            children[i].playable.expectedProgramSemanticFingerprint =
                properties[soundIndex].programSemanticFingerprint;
            children[i].playable.contentRevision = revisions[soundIndex];
            children[i].playable.routeStableId = VN_BUS_MASTER;
            children[i].playable.compatibilityFingerprint = soundIndex + 1u;
            occurrences[i].parentKind =
                harken::playableproperties::ContentPropertyKindV1::Container;
            occurrences[i].stableOccurrenceOrdinal = i + 1u;
            occurrences[i].parentStableId = harkentest::ObjectId(kContainer);
            occurrences[i].parentProgramSemanticFingerprint =
                properties[2].programSemanticFingerprint;
            occurrences[i].childPlayable = children[i].playable;
            occurrences[i].payload = EmptyOccurrencePayload();
            occurrences[i].occurrenceSemanticFingerprint =
                harken::ComputePlayableOccurrenceFingerprintV1(
                    occurrences[i]);
        }

        const harken::GridAxisV1 discreteAxis{1, 0, kDiscrete};
        const std::array<harken::DiscreteGridCellV1, 2> discreteCells{{
            {harken::DiscreteCellKindV1::Exact, 3},
            {harken::DiscreteCellKindV1::Exact, 7},
        }};
        std::array<harken::DiscreteGridRowViewV1, 2> discreteRows{};
        for (std::uint32_t i = 0; i < discreteRows.size(); ++i) {
            discreteRows[i] = {i + 1u, i, i + 1u, 1,
                               &discreteCells[i]};
        }
        const harken::GridAxisV1 continuousAxis{1, 0, kContinuous};
        std::array<harken::ContinuousGridCellV1, 2> continuousCells{};
        continuousCells[0].kind = harken::ContinuousCellKindV1::Interval;
        continuousCells[0].lower = 0.0f;
        continuousCells[0].upper = 4.0f;
        continuousCells[0].lowerInclusive = 1;
        continuousCells[0].upperInclusive = 1;
        continuousCells[1].kind = harken::ContinuousCellKindV1::Interval;
        continuousCells[1].lower = 5.0f;
        continuousCells[1].upper = 8.0f;
        continuousCells[1].lowerInclusive = 1;
        continuousCells[1].upperInclusive = 1;
        std::array<harken::ContinuousGridRowViewV1, 2> continuousRows{};
        for (std::uint32_t i = 0; i < continuousRows.size(); ++i) {
            continuousRows[i] = {i + 1u, i, i + 3u, 1,
                                 &continuousCells[i]};
        }
        std::array<harken::CurveV1, 2> transitionCurves{};
        for (std::uint32_t i = 0; i < transitionCurves.size(); ++i) {
            transitionCurves[i].curveLocalId = i + 1u;
            transitionCurves[i].curve.pointCount = 2;
            transitionCurves[i].curve.points[0] = {0.0f, 0.0f};
            transitionCurves[i].curve.points[1] = {1.0f, 1.0f};
        }
        harken::TransitionRuleViewV1 transitionRule{};
        transitionRule.stableRuleOrdinal = 1;
        transitionRule.sourceAny = 1;
        transitionRule.targetAny = 1;
        transitionRule.sourceFade.active = 1;
        transitionRule.sourceFade.durationFrames = 64;
        transitionRule.sourceFade.curveLocalId =
            transitionCurves[0].curveLocalId;
        transitionRule.targetFade.active = 1;
        transitionRule.targetFade.durationFrames = 64;
        transitionRule.targetFade.curveLocalId =
            transitionCurves[1].curveLocalId;
        harken::TransitionProgramViewV1 transition{};
        transition.ownerContainerStableId = harkentest::ObjectId(kContainer);
        transition.ruleCount = 1;
        transition.defaultRuleIndex = 0;
        transition.rules = &transitionRule;
        harken::ContainerProgramViewV3 container{};
        container.containerStableId = harkentest::ObjectId(kContainer);
        container.programSemanticFingerprint =
            properties[2].programSemanticFingerprint;
        container.mode = harken::ContainerModeV1::VariableControl;
        container.memoryScope = harken::MemoryScopeV1::Emitter;
        container.discreteTiming = 1;
        container.evaluationPeriodFrames = 64;
        container.envelopeFrames = 64;
        // Continuous members remain live even when their current weight is zero.
        container.maxActiveRows = 2;
        container.maxLiveRows = 2;
        container.childCount = children.size();
        container.children = children.data();
        container.discreteAxisCount = 1;
        container.discreteAxes = &discreteAxis;
        container.discreteRowCount = discreteRows.size();
        container.discreteRows = discreteRows.data();
        container.continuousAxisCount = 1;
        container.continuousAxes = &continuousAxis;
        container.continuousRowCount = continuousRows.size();
        container.continuousRows = continuousRows.data();
        container.curveCount = transitionCurves.size();
        container.curves = transitionCurves.data();
        container.transitionProgramIndex = 0;
        harken::ContainerWorldInstallViewV1 world{};
        world.variables = variables.data();
        world.variableCount = variables.size();
        world.playableProperties = properties.data();
        world.playablePropertyCount = properties.size();
        world.playableOccurrences = occurrences.data();
        world.playableOccurrenceCount = occurrences.size();
        world.containers = &container;
        world.containerCount = 1;
        world.transitions = &transition;
        world.transitionCount = 1;
        world.identity.capabilityMask =
            harken::kWorldCapabilityVariableDefinitions |
            harken::kWorldCapabilityPlayableProperties |
            harken::kWorldCapabilityVariableControl |
            harken::kWorldCapabilityTransition;
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harken::InstallContainerWorldV1(*engine, world).status);
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        const VnPCMDesc pcmA = harkentest::MonoDesc(samplesA, 48000);
        const VnPCMDesc pcmB = harkentest::MonoDesc(samplesB, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSoundA, &pcmA));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSoundB, &pcmB));

        VnPreviewPlayPlayableDescV2 desc{};
        desc.structSize = sizeof(desc);
        desc.playableKind = VN_PREVIEW_PLAYABLE_KIND_CONTAINER;
        desc.stableId = harkentest::ObjectId(kContainer);
        VnPreviewPlayableHandle handle = 0;
        ASSERT_EQ(VN_PREVIEW_OK,
                  VnPreview_PlayPlayable(&desc, &handle));
        const std::uint64_t internalHandle =
            handle & ~(std::uint64_t{1} << 63);
        bool sawActive = false;
        bool sawTransitionPrepared = false;
        bool sawTransitionToken = false;
        bool sawTransitionLocked = false;
        std::uint64_t lastTransitionFrame = 0;
        for (std::uint32_t block = 0; block < 32; ++block) {
            if (block == 8) {
                ASSERT_EQ(VN_OK, Vn_SetValue(kDiscrete, 7.0f));
            }
            if (block == 16) {
                ASSERT_EQ(VN_OK, Vn_SetValue(kContinuous, 7.0f));
            }
            VnPreviewPlayableStateV2 state{};
            state.structSize = sizeof(state);
            ASSERT_EQ(VN_PREVIEW_OK,
                      VnPreview_QueryPlayable(handle, &state));
            sawActive = sawActive ||
                (state.lifecycle == VN_PREVIEW_LIFECYCLE_ACTIVE &&
                 state.childLiveCount != 0);
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
            for (const harken::EnginePlayableServiceRow& row :
                 engine->playableServices) {
                if (row.phase == harken::EnginePlayableStartPhase::Free ||
                    row.rootInstance != internalHandle) {
                    continue;
                }
                sawTransitionPrepared = sawTransitionPrepared ||
                    row.variableControlDiscreteRetargetPrepared;
                lastTransitionFrame =
                    row.variableControlDiscreteRetargetFrame;
                if (!row.variableControlDiscreteRetargetTransition.Valid()) {
                    continue;
                }
                sawTransitionToken = true;
                harken::TransitionInstanceSnapshot transitionState{};
                sawTransitionLocked = sawTransitionLocked ||
                    (engine->transitionInstances.Capture(
                         row.variableControlDiscreteRetargetTransition,
                         &transitionState) &&
                     transitionState.state ==
                         harken::TransitionAudibleGateState::Locked);
                if (sawTransitionLocked) break;
            }
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        EXPECT_TRUE(sawActive);
        EXPECT_TRUE(sawTransitionLocked)
            << "prepared=" << sawTransitionPrepared
            << " token=" << sawTransitionToken
            << " transitionFrame=" << lastTransitionFrame
            << " renderedFrames="
            << engine->mixer.BlocksRendered() * engine->config.blockFrames
            << " runtimeRecreateRequired="
            << engine->runtimeRecreateRequired;
        ASSERT_EQ(VN_PREVIEW_OK, VnPreview_StopPlayable(handle, 0));
        VnPreviewPlayableStateV2 stopping{};
        stopping.structSize = sizeof(stopping);
        ASSERT_EQ(VN_PREVIEW_OK,
                  VnPreview_QueryPlayable(handle, &stopping));
        EXPECT_EQ(VN_PREVIEW_LIFECYCLE_CANCELLING,
                  stopping.lifecycle);
        bool stale = false;
        for (std::uint32_t block = 0; block < 32 && !stale; ++block) {
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
            ASSERT_EQ(VN_OK, Vn_Update());
            VnPreviewPlayableStateV2 retired{};
            retired.structSize = sizeof(retired);
            const VnPreviewResult query =
                VnPreview_QueryPlayable(handle, &retired);
            stale = query == VN_PREVIEW_STALE_HANDLE;
            ASSERT_TRUE(stale || query == VN_PREVIEW_OK);
        }
        EXPECT_TRUE(stale);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);
    const auto traceBytes = ReadFileBytes(tracePath);
    ASSERT_GT(traceBytes.size(), harken::kTraceFixedHeaderBytes);
    const auto readU16 = [&](std::size_t offset) {
        std::uint16_t value = 0;
        std::memcpy(&value, traceBytes.data() + offset, sizeof(value));
        return value;
    };
    const auto readU32 = [&](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, traceBytes.data() + offset, sizeof(value));
        return value;
    };
    EXPECT_EQ(harken::kTraceFormatVersion, readU32(4));
    std::size_t recordOffset = readU32(8);
    bool foundWorldOrigin = false;
    while (recordOffset + harken::kTraceRecordHeaderBytes <=
           traceBytes.size()) {
        const std::uint32_t payloadBytes = readU32(recordOffset);
        const std::uint16_t type = readU16(recordOffset + 4);
        const std::size_t payloadOffset =
            recordOffset + harken::kTraceRecordHeaderBytes;
        ASSERT_LE(payloadOffset + payloadBytes, traceBytes.size());
        if (type == harken::kTraceRecordCurrentWorldInstall) {
            ASSERT_GE(payloadBytes, 4u);
            EXPECT_EQ(harken::kCurrentWorldShippingBodyVintage,
                      readU32(payloadOffset));
            foundWorldOrigin = true;
            break;
        }
        recordOffset = payloadOffset + payloadBytes;
    }
    EXPECT_TRUE(foundWorldOrigin);
    const HostRun run1 =
        RunHost("trace-replay " + tracePath + " --capture " + out1);
    ASSERT_EQ(0, run1.exitCode) << run1.output;
    const HostRun run2 =
        RunHost("trace-replay " + tracePath + " --capture " + out2);
    ASSERT_EQ(0, run2.exitCode) << run2.output;
    const auto replay1 = ReadFileBytes(out1);
    const auto replay2 = ReadFileBytes(out2);
    ASSERT_FALSE(replay1.empty());
    EXPECT_EQ(replay1, replay2);
    EXPECT_EQ(original, replay1);
}
#endif

struct ApiRecordLocation {
    std::size_t recordAt = 0;
    std::size_t payloadAt = 0;
    std::uint32_t payloadBytes = 0;
};

struct RecordLocation {
    std::size_t recordAt = 0;
    std::size_t payloadAt = 0;
    std::uint32_t payloadBytes = 0;
    std::uint16_t type = 0;
    std::uint64_t ordinal = 0;
};

// Runs harken_host with the given arguments, capturing combined output.
// Repository paths carry no spaces, so the command line needs no quoting
// (which keeps cmd.exe's quote stripping out of the picture).
HostRun RunHost(const std::string& args) {
    HostRun run;
    const std::string cmd =
        std::string(HARKEN_HOST_PATH) + " " + args + " 2>&1";
    FILE* pipe = _popen(cmd.c_str(), "rt");
    if (pipe == nullptr) {
        return run;
    }
    char chunk[512];
    while (std::fgets(chunk, sizeof chunk, pipe) != nullptr) {
        run.output += chunk;
    }
    run.exitCode = _pclose(pipe);
    return run;
}

std::vector<std::uint8_t> ReadFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes;
    if (in) {
        bytes.assign((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    }
    return bytes;
}

void WriteFileBytes(const std::string& path,
                    const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

bool FindApiRecord(const std::vector<std::uint8_t>& bytes,
                   std::uint16_t wantCallId, std::uint32_t occurrence,
                   ApiRecordLocation* out) {
    if (bytes.size() < harken::kTraceFixedHeaderBytes || out == nullptr) {
        return false;
    }
    std::uint32_t headerBytes = 0;
    std::memcpy(&headerBytes, bytes.data() + 8, sizeof headerBytes);
    std::size_t offset = headerBytes;
    while (offset + harken::kTraceRecordHeaderBytes <= bytes.size()) {
        std::uint32_t payloadBytes = 0;
        std::uint16_t type = 0;
        std::memcpy(&payloadBytes, bytes.data() + offset,
                    sizeof payloadBytes);
        std::memcpy(&type, bytes.data() + offset + 4, sizeof type);
        const std::size_t payloadAt =
            offset + harken::kTraceRecordHeaderBytes;
        if (bytes.size() - payloadAt < payloadBytes) {
            return false;
        }
        if (type == harken::kTraceRecordApiCall && payloadBytes >= 4) {
            std::uint16_t callId = 0;
            std::memcpy(&callId, bytes.data() + payloadAt,
                        sizeof callId);
            if (callId == wantCallId) {
                if (occurrence == 0) {
                    *out = {offset, payloadAt, payloadBytes};
                    return true;
                }
                --occurrence;
            }
        }
        offset = payloadAt + payloadBytes;
    }
    return false;
}

bool ScanRecords(const std::vector<std::uint8_t>& bytes,
                 std::vector<RecordLocation>* out) {
    if (out == nullptr || bytes.size() < harken::kTraceFixedHeaderBytes) {
        return false;
    }
    out->clear();
    std::uint32_t headerBytes = 0;
    std::memcpy(&headerBytes, bytes.data() + 8, sizeof headerBytes);
    if (headerBytes < harken::kTraceFixedHeaderBytes ||
        headerBytes > bytes.size()) {
        return false;
    }
    std::size_t offset = headerBytes;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < harken::kTraceRecordHeaderBytes) {
            return false;
        }
        RecordLocation record;
        record.recordAt = offset;
        std::memcpy(&record.payloadBytes, bytes.data() + offset,
                    sizeof record.payloadBytes);
        std::memcpy(&record.type, bytes.data() + offset + 4,
                    sizeof record.type);
        std::memcpy(&record.ordinal, bytes.data() + offset + 8,
                    sizeof record.ordinal);
        record.payloadAt = offset + harken::kTraceRecordHeaderBytes;
        if (bytes.size() - record.payloadAt < record.payloadBytes) {
            return false;
        }
        out->push_back(record);
        offset = record.payloadAt + record.payloadBytes;
    }
    return true;
}

bool FindRecord(const std::vector<std::uint8_t>& bytes,
                std::uint16_t type, std::uint32_t occurrence,
                RecordLocation* out) {
    std::vector<RecordLocation> records;
    if (out == nullptr || !ScanRecords(bytes, &records)) {
        return false;
    }
    for (const RecordLocation& record : records) {
        if (record.type != type) {
            continue;
        }
        if (occurrence == 0) {
            *out = record;
            return true;
        }
        --occurrence;
    }
    return false;
}

TEST(TraceReplayE2E,
     CurrentWorldV9RawReadableGroupFailureReplaysWithoutMutation) {
    const std::string tracePath = TempPath("current_world_v9_raw_group_failure.vtrc");
    std::filesystem::remove(tracePath);
    std::uint64_t priorGeneration = 0;
    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::PlayablePropertyProgramViewV1 property;
        property.contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Sound;
        property.stableId = harkentest::ObjectId(0x9800);
        property.contentRevision = 1;
        property.routeStableId = VN_BUS_MASTER;
        property.compatibilityFingerprint = 1;
        property.payload = ResolvedDefinitionPayload();
        property.programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(property);
        harken::ContainerWorldInstallViewV1 world;
        world.playableProperties = &property;
        world.playablePropertyCount = 1;
        world.identity.capabilityMask = harken::kWorldCapabilityPlayableProperties;
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
        const auto installed = harken::InstallContainerWorldV1(*session.Engine(), world);
        ASSERT_EQ(harken::WorldInstallStatus::Ok, installed.status);
        priorGeneration = installed.resultGeneration;
        ASSERT_NE(0u, priorGeneration);
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        property.payload.presentMask &= ~UINT64_C(0x80);
        property.programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(property);
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
        const auto refused = harken::InstallContainerWorldV1(*session.Engine(), world);
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld, refused.status);
        EXPECT_EQ(priorGeneration, refused.priorGeneration);
        EXPECT_EQ(0u, refused.resultGeneration);
        EXPECT_EQ(priorGeneration, session.Engine()->variables.CurrentWorldGeneration());
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    const auto bytes = ReadFileBytes(tracePath);
    RecordLocation failed;
    ASSERT_TRUE(FindRecord(bytes, harken::kTraceRecordCurrentWorldInstall, 1, &failed));
    RecordLocation unexpected;
    EXPECT_FALSE(FindRecord(bytes, harken::kTraceRecordCurrentWorldInstall, 2, &unexpected));
    constexpr std::size_t prefixBytes = 32;
    ASSERT_GT(failed.payloadBytes, prefixBytes);
    std::uint16_t flags = 0;
    std::memcpy(&flags, bytes.data() + failed.recordAt + 6, sizeof(flags));
    EXPECT_EQ(0u, flags & harken::kTraceFlagArgsUnreplayable);
    EXPECT_NE(0u, flags & harken::kTraceFlagRawReadableAttempt);
    const auto readU32 = [&](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + failed.payloadAt + offset, sizeof(value));
        return value;
    };
    EXPECT_EQ(harken::kCurrentWorldShippingBodyVintage, readU32(0));
    EXPECT_EQ(1u, readU32(4));
    EXPECT_EQ(static_cast<std::uint32_t>(harken::WorldInstallStatus::InvalidWorld), readU32(8));
    const auto* body = bytes.data() + failed.payloadAt + prefixBytes;
    const auto bodyBytes = failed.payloadBytes - prefixBytes;
    harken::TraceCurrentWorldImage decoded;
    ASSERT_TRUE(decoded.Decode(body, bodyBytes, harken::kCurrentWorldShippingBodyVintage,
                               harken::CurrentWorldDecodeMode::ReplayCurrentAttempt));
    ASSERT_NE(nullptr, decoded.ProductionView());
    ASSERT_EQ(1u, decoded.ProductionView()->playablePropertyCount);
    EXPECT_EQ(UINT64_C(0x12f60), decoded.ProductionView()->playableProperties[0].payload.presentMask);
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(*decoded.ProductionView()));
    EXPECT_FALSE(decoded.Decode(body, bodyBytes, harken::kCurrentWorldShippingBodyVintage,
                                harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(UINT64_C(0x12f60), decoded.ProductionView()->playableProperties[0].payload.presentMask);
    for (int runIndex = 0; runIndex < 2; ++runIndex) {
        const auto replay = RunHost("trace-replay " + tracePath);
        EXPECT_EQ(0, replay.exitCode) << replay.output;
    }
}

TEST(TraceReplayE2E,
     CurrentWorldIoFailureRecordsOneSelfContainedReplayableCall) {
    const std::string tracePath =
        TempPath("current_world_io_failure.vtrc");
    const std::string missingWorld =
        TempPath("current_world_io_failure_missing.vnworld");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(missingWorld);

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = missingWorld.c_str();
        VnCurrentWorldLoadReport report{};
        report.structSize = sizeof(report);
        ASSERT_EQ(VN_CURRENT_WORLD_IO_ERROR,
                  Vn_LoadCurrentWorld(&desc, &report));
        ASSERT_EQ(VN_CURRENT_WORLD_LOAD_IO_ERROR, report.status);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    std::vector<RecordLocation> records;
    ASSERT_TRUE(ScanRecords(bytes, &records));
    std::uint32_t loadCalls = 0;
    std::uint32_t nestedInstalls = 0;
    for (const RecordLocation& record : records) {
        if (record.type == harken::kTraceRecordCurrentWorldInstall) {
            ++nestedInstalls;
        }
        if (record.type != harken::kTraceRecordApiCall ||
            record.payloadBytes < sizeof(std::uint16_t)) {
            continue;
        }
        std::uint16_t callId = 0;
        std::memcpy(&callId, bytes.data() + record.payloadAt,
                    sizeof(callId));
        if (callId == 98) {
            ++loadCalls;
        }
    }
    EXPECT_EQ(1u, loadCalls);
    EXPECT_EQ(0u, nestedInstalls);

    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E,
     CurrentWorldSuccessRecordsOneSelfContainedReplayableCall) {
    const std::string tracePath =
        TempPath("current_world_success.vtrc");
    const std::string worldPath =
        TempPath("current_world_success.vnworld");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(worldPath);

    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9801), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &definition;
    world.variableCount = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harken::EncodeCurrentWorldArtifactV1(world, artifact));
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(worldPath, artifact));

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = worldPath.c_str();
        VnCurrentWorldLoadReport report{};
        report.structSize = sizeof(report);
        ASSERT_EQ(VN_OK, Vn_LoadCurrentWorld(&desc, &report));
        ASSERT_EQ(VN_CURRENT_WORLD_LOAD_OK, report.status);
        ASSERT_EQ(world.identity.runtimeSemanticFingerprint,
                  report.runtimeSemanticFingerprint);
        ASSERT_EQ(world.identity.capabilityMask, report.capabilityMask);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    std::vector<RecordLocation> records;
    ASSERT_TRUE(ScanRecords(bytes, &records));
    std::uint32_t loadCalls = 0;
    std::uint32_t nestedInstalls = 0;
    for (const RecordLocation& record : records) {
        if (record.type == harken::kTraceRecordCurrentWorldInstall) {
            ++nestedInstalls;
        }
        if (record.type != harken::kTraceRecordApiCall ||
            record.payloadBytes < sizeof(std::uint16_t)) {
            continue;
        }
        std::uint16_t callId = 0;
        std::memcpy(&callId, bytes.data() + record.payloadAt,
                    sizeof(callId));
        if (callId == harken::kTraceCallLoadCurrentWorld) {
            ++loadCalls;
        }
    }
    EXPECT_EQ(1u, loadCalls);
    EXPECT_EQ(0u, nestedInstalls);

    ASSERT_TRUE(std::filesystem::remove(worldPath));
    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E,
     CurrentWorldTrailingDataRecordsOnlyHeaderAndReplaysSizeMismatch) {
    const std::string tracePath =
        TempPath("current_world_trailing_data.vtrc");
    const std::string worldPath =
        TempPath("current_world_trailing_data.vnworld");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(worldPath);

    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9802), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &definition;
    world.variableCount = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harken::EncodeCurrentWorldArtifactV1(world, artifact));
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(worldPath, artifact));
    std::error_code resizeError;
    const std::uint64_t hostileBytes =
        harken::kCurrentWorldArtifactMaxBodyBytes +
        harken::kCurrentWorldArtifactHeaderBytes + 4096;
    std::filesystem::resize_file(worldPath, hostileBytes, resizeError);
    ASSERT_FALSE(resizeError) << resizeError.message();

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = worldPath.c_str();
        VnCurrentWorldLoadReport report{};
        report.structSize = sizeof(report);
        ASSERT_EQ(VN_BAD_CURRENT_WORLD,
                  Vn_LoadCurrentWorld(&desc, &report));
        ASSERT_EQ(VN_CURRENT_WORLD_LOAD_SIZE_MISMATCH, report.status);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    EXPECT_LT(bytes.size(), 4096u);
    std::vector<RecordLocation> records;
    ASSERT_TRUE(ScanRecords(bytes, &records));
    std::uint32_t loadCalls = 0;
    std::uint32_t nestedInstalls = 0;
    for (const RecordLocation& record : records) {
        if (record.type == harken::kTraceRecordCurrentWorldInstall) {
            ++nestedInstalls;
        }
        if (record.type != harken::kTraceRecordApiCall ||
            record.payloadBytes < sizeof(std::uint16_t)) {
            continue;
        }
        std::uint16_t callId = 0;
        std::memcpy(&callId, bytes.data() + record.payloadAt,
                    sizeof(callId));
        if (callId == harken::kTraceCallLoadCurrentWorld) {
            ++loadCalls;
        }
    }
    EXPECT_EQ(1u, loadCalls);
    EXPECT_EQ(0u, nestedInstalls);

    ASSERT_TRUE(std::filesystem::remove(worldPath));
    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E,
     CurrentWorldOverLimitDeclarationRecordsOnlyHeaderAndReplaysTooLarge) {
    const std::string tracePath =
        TempPath("current_world_too_large.vtrc");
    const std::string worldPath =
        TempPath("current_world_too_large.vnworld");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(worldPath);

    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9803), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &definition;
    world.variableCount = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harken::EncodeCurrentWorldArtifactV1(world, artifact));
    const std::uint64_t declaredBodyBytes =
        harken::kCurrentWorldArtifactMaxBodyBytes + 1;
    std::memcpy(artifact.data() + 24, &declaredBodyBytes,
                sizeof declaredBodyBytes);
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(worldPath, artifact));

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = worldPath.c_str();
        VnCurrentWorldLoadReport report{};
        report.structSize = sizeof(report);
        ASSERT_EQ(VN_BAD_CURRENT_WORLD,
                  Vn_LoadCurrentWorld(&desc, &report));
        ASSERT_EQ(VN_CURRENT_WORLD_LOAD_TOO_LARGE, report.status);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    EXPECT_LT(bytes.size(), 4096u);
    ApiRecordLocation load{};
    ASSERT_TRUE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 0, &load));
    EXPECT_FALSE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 1, &load));
    RecordLocation nested{};
    EXPECT_FALSE(FindRecord(
        bytes, harken::kTraceRecordCurrentWorldInstall, 0, &nested));

    ASSERT_TRUE(std::filesystem::remove(worldPath));
    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E,
     CurrentWorldCompleteMalformedArtifactReplaysFirstFailure) {
    const std::string tracePath =
        TempPath("current_world_checksum_failure.vtrc");
    const std::string worldPath =
        TempPath("current_world_checksum_failure.vnworld");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(worldPath);

    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9804), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &definition;
    world.variableCount = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harken::EncodeCurrentWorldArtifactV1(world, artifact));
    ASSERT_GT(artifact.size(), harken::kCurrentWorldArtifactHeaderBytes);
    artifact.back() ^= 1;
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(worldPath, artifact));

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = worldPath.c_str();
        VnCurrentWorldLoadReport report{};
        report.structSize = sizeof(report);
        ASSERT_EQ(VN_BAD_CURRENT_WORLD,
                  Vn_LoadCurrentWorld(&desc, &report));
        ASSERT_EQ(VN_CURRENT_WORLD_LOAD_CHECKSUM_MISMATCH, report.status);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    ApiRecordLocation load{};
    ASSERT_TRUE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 0, &load));
    EXPECT_FALSE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 1, &load));
    RecordLocation nested{};
    EXPECT_FALSE(FindRecord(
        bytes, harken::kTraceRecordCurrentWorldInstall, 0, &nested));

    ASSERT_TRUE(std::filesystem::remove(worldPath));
    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E,
     CurrentWorldShortArtifactPreservesPrefixAndReplaysTruncated) {
    const std::string tracePath =
        TempPath("current_world_short.vtrc");
    const std::string worldPath =
        TempPath("current_world_short.vnworld");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(worldPath);
    std::vector<std::uint8_t> prefix(31, 0xA5);
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(worldPath, prefix));

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = worldPath.c_str();
        VnCurrentWorldLoadReport report{};
        report.structSize = sizeof(report);
        ASSERT_EQ(VN_BAD_CURRENT_WORLD,
                  Vn_LoadCurrentWorld(&desc, &report));
        ASSERT_EQ(VN_CURRENT_WORLD_LOAD_TRUNCATED, report.status);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    ApiRecordLocation load{};
    ASSERT_TRUE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 0, &load));
    EXPECT_FALSE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 1, &load));

    ASSERT_TRUE(std::filesystem::remove(worldPath));
    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E,
     CurrentWorldBodyCapacityFailureReplaysWithoutAllocatingBody) {
    const std::string tracePath =
        TempPath("current_world_capacity.vtrc");
    const std::string worldPath =
        TempPath("current_world_capacity.vnworld");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(worldPath);

    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9805), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &definition;
    world.variableCount = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harken::EncodeCurrentWorldArtifactV1(world, artifact));
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(worldPath, artifact));

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        harken::detail::ArmLogicAllocFaultForTests(
            1, harken::detail::LogicAllocSite::CurrentWorldArtifactBody);
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = worldPath.c_str();
        VnCurrentWorldLoadReport report{};
        report.structSize = sizeof(report);
        EXPECT_EQ(VN_CURRENT_WORLD_CAPACITY,
                  Vn_LoadCurrentWorld(&desc, &report));
        EXPECT_EQ(VN_CURRENT_WORLD_LOAD_CAPACITY, report.status);
        harken::detail::DisarmLogicAllocFaultForTests();
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    ApiRecordLocation load{};
    ASSERT_TRUE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 0, &load));
    EXPECT_FALSE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 1, &load));
    RecordLocation nested{};
    EXPECT_FALSE(FindRecord(
        bytes, harken::kTraceRecordCurrentWorldInstall, 0, &nested));

    ASSERT_TRUE(std::filesystem::remove(worldPath));
    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E,
     CurrentWorldDecodedImageCapacityFailureReplaysWithoutAllocation) {
    const std::string tracePath =
        TempPath("current_world_decoded_image_capacity.vtrc");
    const std::string worldPath =
        TempPath("current_world_decoded_image_capacity.vnworld");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(worldPath);

    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9809), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &definition;
    world.variableCount = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harken::EncodeCurrentWorldArtifactV1(world, artifact));
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(worldPath, artifact));

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        harken::detail::ArmLogicAllocFaultForTests(
            1, harken::detail::LogicAllocSite::CurrentWorldDecodedImage);
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = worldPath.c_str();
        VnCurrentWorldLoadReport report{};
        report.structSize = sizeof(report);
        EXPECT_EQ(VN_CURRENT_WORLD_CAPACITY,
                  Vn_LoadCurrentWorld(&desc, &report));
        EXPECT_EQ(VN_CURRENT_WORLD_LOAD_CAPACITY, report.status);
        EXPECT_EQ(0u, report.resultGeneration);
        EXPECT_EQ(0u, session.Engine()->variables.CurrentWorldGeneration());
        harken::detail::DisarmLogicAllocFaultForTests();
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    ApiRecordLocation load{};
    ASSERT_TRUE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 0, &load));
    EXPECT_FALSE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 1, &load));
    RecordLocation nested{};
    EXPECT_FALSE(FindRecord(
        bytes, harken::kTraceRecordCurrentWorldInstall, 0, &nested));

    ASSERT_TRUE(std::filesystem::remove(worldPath));
    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E,
     CurrentWorldCanonicalInstallCapacityRefusalReplaysWithoutMutation) {
    const std::string tracePath =
        TempPath("current_world_install_capacity.vtrc");
    const std::string worldPath =
        TempPath("current_world_install_capacity.vnworld");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(worldPath);

    std::vector<harken::VariableDefinitionV1> definitions;
    definitions.reserve(harken::kVariableDefinitionCapacity + 1);
    for (std::uint32_t i = 0;
         i < harken::kVariableDefinitionCapacity + 1; ++i) {
        definitions.push_back(harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9808 + i), harken::VariableKindV1::Smooth,
            0.0f, 1.0f, 0.5f));
    }
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = definitions.data();
    world.variableCount = static_cast<std::uint32_t>(definitions.size());
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harken::EncodeCurrentWorldArtifactV1(world, artifact));
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(worldPath, artifact));

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = worldPath.c_str();
        VnCurrentWorldLoadReport report{};
        report.structSize = sizeof(report);
        EXPECT_EQ(VN_CURRENT_WORLD_CAPACITY,
                  Vn_LoadCurrentWorld(&desc, &report));
        EXPECT_EQ(VN_CURRENT_WORLD_LOAD_CAPACITY, report.status);
        EXPECT_EQ(0u, report.resultGeneration);
        EXPECT_EQ(0u, session.Engine()->variables.CurrentWorldGeneration());
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    ApiRecordLocation load{};
    ASSERT_TRUE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 0, &load));
    RecordLocation nested{};
    EXPECT_FALSE(FindRecord(
        bytes, harken::kTraceRecordCurrentWorldInstall, 0, &nested));

    ASSERT_TRUE(std::filesystem::remove(worldPath));
    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E,
     CurrentWorldBadHeaderPrefixReplaysFirstFailureWithoutSourceFile) {
    const std::string tracePath =
        TempPath("current_world_bad_header.vtrc");
    const std::string worldPath =
        TempPath("current_world_bad_header.vnworld");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(worldPath);

    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9806), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &definition;
    world.variableCount = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harken::EncodeCurrentWorldArtifactV1(world, artifact));
    artifact[0] = 'X';
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(worldPath, artifact));

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = worldPath.c_str();
        VnCurrentWorldLoadReport report{};
        report.structSize = sizeof(report);
        ASSERT_EQ(VN_BAD_CURRENT_WORLD,
                  Vn_LoadCurrentWorld(&desc, &report));
        ASSERT_EQ(VN_CURRENT_WORLD_LOAD_BAD_MAGIC, report.status);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    EXPECT_LT(bytes.size(), 4096u);
    ApiRecordLocation load{};
    ASSERT_TRUE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 0, &load));

    ASSERT_TRUE(std::filesystem::remove(worldPath));
    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E,
     CurrentWorldReplayRejectsInvalidImageVocabularyAndCaptureLength) {
    const std::string tracePath =
        TempPath("current_world_mutation_source.vtrc");
    const std::string worldPath =
        TempPath("current_world_mutation_source.vnworld");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(worldPath);

    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9807), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &definition;
    world.variableCount = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harken::EncodeCurrentWorldArtifactV1(world, artifact));
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(worldPath, artifact));

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = worldPath.c_str();
        VnCurrentWorldLoadReport report{};
        report.structSize = sizeof(report);
        ASSERT_EQ(VN_OK, Vn_LoadCurrentWorld(&desc, &report));
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    ASSERT_TRUE(std::filesystem::remove(worldPath));

    const std::vector<std::uint8_t> source = ReadFileBytes(tracePath);
    ApiRecordLocation load{};
    ASSERT_TRUE(FindApiRecord(
        source, harken::kTraceCallLoadCurrentWorld, 0, &load));
    std::uint32_t pathBytes = 0;
    const std::size_t pathBytesAt = load.payloadAt + 8;
    ASSERT_LE(pathBytesAt + sizeof(pathBytes), source.size());
    std::memcpy(&pathBytes, source.data() + pathBytesAt,
                sizeof pathBytes);
    const std::size_t imageTagAt =
        pathBytesAt + sizeof(pathBytes) + pathBytes + sizeof(std::uint32_t) +
        sizeof(VnCurrentWorldLoadReport);
    const std::size_t dispositionAt = imageTagAt + sizeof(std::uint32_t);
    const std::size_t capturedBytesAt = imageTagAt + 32;
    ASSERT_LE(capturedBytesAt + sizeof(std::uint64_t), source.size());

    const auto expectMalformed = [&](const std::string& name,
                                     std::vector<std::uint8_t> bytes) {
        const std::string path = TempPath(name);
        ASSERT_NO_FATAL_FAILURE(WriteFileBytes(path, bytes));
        const HostRun run = RunHost("trace-replay " + path);
        EXPECT_EQ(1, run.exitCode) << run.output;
    };

    std::vector<std::uint8_t> invalidTag = source;
    const std::uint32_t unknownTag = UINT32_MAX;
    std::memcpy(invalidTag.data() + imageTagAt, &unknownTag,
                sizeof unknownTag);
    expectMalformed("current_world_invalid_tag.vtrc", invalidTag);

    std::vector<std::uint8_t> invalidDisposition = source;
    const std::uint32_t completeArtifact = static_cast<std::uint32_t>(
        harken::TraceCurrentWorldCaptureDispositionV1::CompleteArtifact);
    std::memcpy(invalidDisposition.data() + dispositionAt,
                &completeArtifact, sizeof completeArtifact);
    expectMalformed("current_world_invalid_disposition.vtrc",
                    invalidDisposition);

    std::vector<std::uint8_t> overLimit = source;
    const std::uint64_t hostileCapturedBytes =
        harken::kCurrentWorldArtifactMaxBodyBytes +
        harken::kCurrentWorldArtifactHeaderBytes + 1;
    std::memcpy(overLimit.data() + capturedBytesAt, &hostileCapturedBytes,
                sizeof hostileCapturedBytes);
    expectMalformed("current_world_overlimit_capture.vtrc", overLimit);
}

TEST(TraceReplayE2E,
     CurrentWorldReplayMapsIdempotentAndReplacementGenerations) {
    const std::string tracePath =
        TempPath("current_world_generation_map.vtrc");
    const std::string firstPath =
        TempPath("current_world_generation_first.vnworld");
    const std::string secondPath =
        TempPath("current_world_generation_second.vnworld");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(firstPath);
    std::filesystem::remove(secondPath);

    const harken::VariableDefinitionV1 firstDefinition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9810), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    const harken::VariableDefinitionV1 secondDefinition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9811), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.25f);
    const auto makeArtifact = [](const harken::VariableDefinitionV1& definition) {
        harken::ContainerWorldInstallViewV1 world{};
        world.variables = &definition;
        world.variableCount = 1;
        world.identity.capabilityMask =
            harken::kWorldCapabilityVariableDefinitions;
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
        std::vector<std::uint8_t> artifact;
        EXPECT_TRUE(harken::EncodeCurrentWorldArtifactV1(world, artifact));
        return artifact;
    };
    ASSERT_NO_FATAL_FAILURE(
        WriteFileBytes(firstPath, makeArtifact(firstDefinition)));
    ASSERT_NO_FATAL_FAILURE(
        WriteFileBytes(secondPath, makeArtifact(secondDefinition)));

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        const auto load = [](const std::string& path) {
            VnCurrentWorldLoadDesc desc{};
            desc.structSize = sizeof(desc);
            desc.utf8Path = path.c_str();
            VnCurrentWorldLoadReport report{};
            report.structSize = sizeof(report);
            EXPECT_EQ(VN_OK, Vn_LoadCurrentWorld(&desc, &report));
            EXPECT_EQ(VN_CURRENT_WORLD_LOAD_OK, report.status);
            return report;
        };
        const VnCurrentWorldLoadReport first = load(firstPath);
        const VnCurrentWorldLoadReport idempotent = load(firstPath);
        const VnCurrentWorldLoadReport replacement = load(secondPath);
        EXPECT_EQ(first.resultGeneration, idempotent.priorGeneration);
        EXPECT_EQ(first.resultGeneration, idempotent.resultGeneration);
        EXPECT_EQ(first.resultGeneration, replacement.priorGeneration);
        EXPECT_NE(first.resultGeneration, replacement.resultGeneration);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    ApiRecordLocation load{};
    ASSERT_TRUE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 0, &load));
    ASSERT_TRUE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 1, &load));
    ASSERT_TRUE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 2, &load));
    EXPECT_FALSE(FindApiRecord(
        bytes, harken::kTraceCallLoadCurrentWorld, 3, &load));
    RecordLocation nested{};
    EXPECT_FALSE(FindRecord(
        bytes, harken::kTraceRecordCurrentWorldInstall, 0, &nested));

    ASSERT_TRUE(std::filesystem::remove(firstPath));
    ASSERT_TRUE(std::filesystem::remove(secondPath));
    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

bool ResizeRecordPayload(std::vector<std::uint8_t>* bytes,
                         const RecordLocation& record,
                         std::uint32_t newPayloadBytes) {
    if (bytes == nullptr ||
        record.payloadAt + record.payloadBytes > bytes->size()) {
        return false;
    }
    const std::size_t oldEnd =
        record.payloadAt + record.payloadBytes;
    if (newPayloadBytes < record.payloadBytes) {
        bytes->erase(
            bytes->begin() + record.payloadAt + newPayloadBytes,
            bytes->begin() + oldEnd);
    } else if (newPayloadBytes > record.payloadBytes) {
        bytes->insert(
            bytes->begin() + oldEnd,
            newPayloadBytes - record.payloadBytes,
            static_cast<std::uint8_t>(0xA5));
    }
    std::memcpy(bytes->data() + record.recordAt, &newPayloadBytes,
                sizeof newPayloadBytes);
    return true;
}

bool PatchRecordPayloadU64(std::vector<std::uint8_t>* bytes,
                           const RecordLocation& record,
                           std::uint64_t replacement) {
    if (bytes == nullptr ||
        record.payloadBytes < sizeof replacement ||
        record.payloadAt + sizeof replacement > bytes->size()) {
        return false;
    }
    std::memcpy(bytes->data() + record.payloadAt, &replacement,
                sizeof replacement);
    return true;
}

bool PatchRecordOrdinal(std::vector<std::uint8_t>* bytes,
                        const RecordLocation& record,
                        std::uint64_t replacement) {
    if (bytes == nullptr ||
        record.recordAt + harken::kTraceRecordHeaderBytes > bytes->size()) {
        return false;
    }
    std::memcpy(bytes->data() + record.recordAt + 8, &replacement,
                sizeof replacement);
    return true;
}

bool PatchRecordFlags(std::vector<std::uint8_t>* bytes,
                      const RecordLocation& record,
                      std::uint16_t replacement) {
    if (bytes == nullptr ||
        record.recordAt + harken::kTraceRecordHeaderBytes > bytes->size()) {
        return false;
    }
    std::memcpy(bytes->data() + record.recordAt + 6, &replacement,
                sizeof replacement);
    return true;
}

bool ReplaceCurrentWorldRecordBody(
    std::vector<std::uint8_t>* bytes, std::uint32_t occurrence,
    std::uint32_t vintage, const std::vector<std::uint8_t>& body) {
    RecordLocation record{};
    if (bytes == nullptr ||
        !FindRecord(*bytes, harken::kTraceRecordCurrentWorldInstall,
                    occurrence, &record) ||
        !ResizeRecordPayload(
            bytes, record, static_cast<std::uint32_t>(32 + body.size()))) {
        return false;
    }
    std::memcpy(bytes->data() + record.payloadAt, &vintage,
                sizeof vintage);
    if (!body.empty()) {
        std::memcpy(bytes->data() + record.payloadAt + 32,
                    body.data(), body.size());
    }
    return true;
}

TEST(TraceReplayE2E,
     HistoricalCurrentWorldBodyIsRejected) {
    const std::string tracePath =
        TempPath("historical_current_world_source.vtrc");
    std::filesystem::remove(tracePath);

    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9820), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &definition;
    world.variableCount = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harken::InstallContainerWorldV1(
                      *session.Engine(), world).status);
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    std::vector<std::uint8_t> historicalBody;
    auto historicalWorld = harkentest::AsHistoricalPrefix(world);
    historicalWorld.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(historicalWorld);
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV6(historicalWorld, historicalBody));
    const std::uint32_t historicalVintage =
        harken::kCurrentWorldBodyVintageV6;
    ASSERT_TRUE(ReplaceCurrentWorldRecordBody(
        &bytes, 0, historicalVintage, historicalBody));
    RecordLocation historicalRecord{};
    ASSERT_TRUE(FindRecord(
        bytes, harken::kTraceRecordCurrentWorldInstall, 0,
        &historicalRecord));
    const std::uint64_t unrelatedPrior = 0xFFFFFFFFFFFFFFFFull;
    std::memcpy(bytes.data() + historicalRecord.payloadAt + 16,
                &unrelatedPrior, sizeof unrelatedPrior);

    const HostRun replay = RunMutatedTrace(
        bytes, "historical_current_world_diagnostic_only.vtrc");
    EXPECT_EQ(1, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E, HistoricalCurrentWorldWithAncestorsIsRejected) {
    harken::PlayablePropertyProgramViewV1 properties[2]{};
    properties[0].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Layer;
    properties[0].stableId = harkentest::ObjectId(11);
    properties[0].payload = ResolvedDefinitionPayload();
    properties[0].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[0]);
    harken::StructuralAncestorRefV1 ancestor{};
    ancestor.contentPropertyKind = properties[0].contentPropertyKind;
    ancestor.stableId = properties[0].stableId;
    ancestor.programSemanticFingerprint = properties[0].programSemanticFingerprint;
    ancestor.payload = harken::MakeStructuralAncestorPayloadV1(properties[0].payload);
    properties[1].stableId = harkentest::ObjectId(12);
    properties[1].structuralAncestorCount = 1;
    properties[1].structuralAncestors = &ancestor;
    properties[1].payload = ResolvedDefinitionPayload();
    harken::HistoricalContainerWorldViewV1 historical{};
    historical.playableProperties = properties;
    historical.playablePropertyCount = 2;
    std::vector<std::uint8_t> body;
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV8(historical, body));

    const auto tracePath = TempPath("retired_ancestor_world.vtrc");
    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harkentest::InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(42)}).status);
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    const auto source = ReadFileBytes(tracePath);
    for (const auto vintage : {harken::kCurrentWorldBodyVintageV7,
                               harken::kCurrentWorldBodyVintageV8}) {
        harken::CurrentWorldBodyImage decoded;
        EXPECT_FALSE(decoded.Decode(body.data(), body.size(), vintage,
                                    harken::CurrentWorldDecodeMode::ReplayRawAttempt));
        EXPECT_EQ(nullptr, decoded.ReplayView());
        EXPECT_EQ(nullptr, decoded.ProductionView());
        auto bytes = source;
        ASSERT_TRUE(ReplaceCurrentWorldRecordBody(&bytes, 0, vintage, body));
        const auto replay = RunMutatedTrace(
            bytes, "retired_ancestor_v" + std::to_string(vintage) + ".vtrc");
        EXPECT_EQ(1, replay.exitCode) << replay.output;
    }
}

TEST(TraceReplayE2E, VariableTicketQueriesRemapWorldAndPreserveUnknown) {
    for (bool unknown : {false, true}) {
        const std::string tracePath = TempPath(
            unknown ? "variable_unknown_ticket.vtrc" : "variable_ticket_world.vtrc");
        std::filesystem::remove(tracePath);
        {
            EngineSession session(MakeNullConfig());
            ASSERT_EQ(VN_OK, session.InitResult());
            ASSERT_EQ(harken::WorldInstallStatus::Ok,
                      harkentest::InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(41)}).status);
            ASSERT_EQ(harken::WorldInstallStatus::Ok,
                      harkentest::InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(42)}).status);
            ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
            VnVariableWriteV1 write{harkentest::ObjectId(42), 0.5f, 0};
            VnVariableBatchDescV1 batch{};
            batch.structSize = sizeof(batch);
            batch.writeCount = 1;
            batch.writes = &write;
            VnVariableTicketQueryDescV1 query{};
            query.structSize = sizeof(query);
            ASSERT_EQ(VN_OK, Vn_SetValues(&batch, &query.ticket));
            if (unknown) {
                query.ticket.sequence += 100000;
            }
            VnVariableTicketQueryResultV1 out{};
            out.structSize = sizeof(out);
            ASSERT_EQ(VN_OK, Vn_QueryVariableTicket(&query, &out));
            ASSERT_EQ(unknown ? VN_VARIABLE_TICKET_UNKNOWN : VN_VARIABLE_TICKET_FOUND,
                      out.status);
            ASSERT_EQ(VN_OK, Vn_TraceStop());
        }
        const HostRun replay = RunMutatedTrace(ReadFileBytes(tracePath),
            unknown ? "replay_unknown_ticket.vtrc" : "replay_ticket_world.vtrc");
        EXPECT_EQ(0, replay.exitCode) << replay.output;
        if (!unknown) {
            auto conflicting = ReadFileBytes(tracePath);
            ApiRecordLocation batchRecord;
            ASSERT_TRUE(FindApiRecord(conflicting, harken::kTraceCallSetValues,
                                      0, &batchRecord));
            constexpr std::size_t kWorldOffset = 40;
            ASSERT_GE(batchRecord.payloadBytes,
                      kWorldOffset + sizeof(std::uint64_t));
            const std::uint64_t wrongWorld = UINT64_MAX;
            std::memcpy(conflicting.data() + batchRecord.payloadAt + kWorldOffset,
                        &wrongWorld, sizeof(wrongWorld));
            const HostRun refused = RunMutatedTrace(
                conflicting, "replay_conflicting_variable_world.vtrc");
            EXPECT_EQ(1, refused.exitCode) << refused.output;
        }
    }
}

TEST(TraceReplayE2E, RejectedVolumeTargetOutcomeRoundTrips) {
    const std::string tracePath = TempPath("variable_rejected_volume.vtrc");
    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harkentest::InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(42)},
                      0.0f, 1.0f, 0.0f).status);
        VnBindingDesc binding{};
        binding.structSize = sizeof(binding);
        binding.variableId = harkentest::ObjectId(42);
        binding.targetScopeKind = VN_TARGET_SCOPE_BUS;
        binding.targetId = harken::kBusTargetBusVolume;
        binding.curve.structSize = sizeof(binding.curve);
        binding.curve.pointCount = 2;
        binding.curve.points[0] = {0.0f, 0.0f};
        binding.curve.points[1] = {1.0f, 800.0f};
        binding.curve.handles[0] = harkentest::EndpointHandles(
            binding.curve.points[0], binding.curve.points[1]);
        ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &binding));
        VnBindingAttachDesc attachment{};
        attachment.structSize = sizeof(attachment);
        attachment.scopeKind = VN_TARGET_SCOPE_BUS;
        attachment.objectId = VN_BUS_MASTER;
        ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1), &attachment));
        ASSERT_EQ(VN_OK, Vn_Update());
        session.Engine()->nullBackend.Pump(2);
        VnVariableWriteV1 write{harkentest::ObjectId(42), 1.0f, 0};
        VnVariableBatchDescV1 batch{};
        batch.structSize = sizeof(batch);
        batch.writeCount = 1;
        batch.writes = &write;
        VnVariableTicketV1 ticket{};
        ASSERT_EQ(VN_OK, Vn_SetValues(&batch, &ticket));
        ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
        VnVariableTicketQueryDescV1 query{};
        query.structSize = sizeof(query);
        query.ticket = ticket;
        VnVariableTicketQueryResultV1 result{};
        result.structSize = sizeof(result);
        ASSERT_EQ(VN_OK, Vn_QueryVariableTicket(&query, &result));
        ASSERT_EQ(VN_VARIABLE_OUTCOME_REJECTED_TARGET, result.outcome);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    const auto bytes = ReadFileBytes(tracePath);
    RecordLocation outcome{};
    ASSERT_TRUE(FindRecord(bytes, harken::kTraceRecordVariableOutcome, 0, &outcome));
    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E, VariableQueryUsesRecordedEmitterIdentity) {
    const std::string tracePath = TempPath("variable_emitter_query.vtrc");
    std::filesystem::remove(tracePath);
    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harkentest::InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(42)},
                      -1.0f, 1.0f, 0.0f).status);
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        VnVariableQueryDescV1 query{};
        query.structSize = sizeof(query);
        query.variableId = harkentest::ObjectId(42);
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&query.emitter));
        VnVariableQueryResultV1 out{};
        out.structSize = sizeof(out);
        ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &out));
        ASSERT_EQ(VN_OK, Vn_DestroyEmitter(query.emitter));
        out.currentValue = 123.0f;
        out.executionFingerprint = UINT64_MAX;
        ASSERT_EQ(VN_STALE_HANDLE, Vn_QueryVariable(&query, &out));
        EXPECT_EQ(sizeof(out), out.structSize);
        EXPECT_FLOAT_EQ(0.0f, out.currentValue);
        EXPECT_EQ(0u, out.executionFingerprint);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    const auto bytes = ReadFileBytes(tracePath);
    ApiRecordLocation record{};
    ASSERT_TRUE(FindApiRecord(bytes, harken::kTraceCallQueryVariable, 0, &record));
    std::uint64_t recordedEmitter = 0;
    ASSERT_GE(record.payloadBytes, 36u);
    std::memcpy(&recordedEmitter, bytes.data() + record.payloadAt + 28,
                sizeof(recordedEmitter));
    EXPECT_EQ(1u, recordedEmitter);
    const HostRun replay = RunMutatedTrace(bytes,
        "replay_variable_emitter_query.vtrc");
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E,
     HistoricalCurrentWorldResultCannotSeedShippingPriorMapping) {
    const std::string tracePath =
        TempPath("historical_current_world_mapping_source.vtrc");
    std::filesystem::remove(tracePath);

    const harken::VariableDefinitionV1 firstDefinition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9830), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    const harken::VariableDefinitionV1 secondDefinition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9831), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.25f);
    harken::ContainerWorldInstallViewV1 firstWorld{};
    firstWorld.variables = &firstDefinition;
    firstWorld.variableCount = 1;
    firstWorld.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    firstWorld.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(firstWorld);
    harken::ContainerWorldInstallViewV1 secondWorld{};
    secondWorld.variables = &secondDefinition;
    secondWorld.variableCount = 1;
    secondWorld.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    secondWorld.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(secondWorld);

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harken::InstallContainerWorldV1(
                      *session.Engine(), firstWorld).status);
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harken::InstallContainerWorldV1(
                      *session.Engine(), secondWorld).status);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    RecordLocation currentWorld{};
    ASSERT_TRUE(FindRecord(
        bytes, harken::kTraceRecordCurrentWorldInstall, 1, &currentWorld));
    EXPECT_FALSE(FindRecord(
        bytes, harken::kTraceRecordCurrentWorldInstall, 2, &currentWorld));
    const HostRun shippingReplay = RunMutatedTrace(
        bytes, "shipping_current_world_generation_mapping.vtrc");
    ASSERT_EQ(0, shippingReplay.exitCode) << shippingReplay.output;
    std::vector<std::uint8_t> historicalBody;
    auto historicalFirst = harkentest::AsHistoricalPrefix(firstWorld);
    historicalFirst.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(historicalFirst);
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV6(historicalFirst, historicalBody));
    ASSERT_TRUE(ReplaceCurrentWorldRecordBody(
        &bytes, 0, harken::kCurrentWorldBodyVintageV6, historicalBody));

    const HostRun replay = RunMutatedTrace(
        bytes, "historical_current_world_no_generation_mapping.vtrc");
    EXPECT_EQ(1, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E,
     HistoricalCurrentWorldBodyDoesNotChangeInstalledShippingWorld) {
    const std::string tracePath =
        TempPath("historical_current_world_install_source.vtrc");
    std::filesystem::remove(tracePath);

    const harken::VariableDefinitionV1 firstDefinition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9840), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    const harken::VariableDefinitionV1 secondDefinition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9841), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.25f);
    harken::ContainerWorldInstallViewV1 firstWorld{};
    firstWorld.variables = &firstDefinition;
    firstWorld.variableCount = 1;
    firstWorld.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    firstWorld.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(firstWorld);
    harken::ContainerWorldInstallViewV1 secondWorld{};
    secondWorld.variables = &secondDefinition;
    secondWorld.variableCount = 1;
    secondWorld.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    secondWorld.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(secondWorld);

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harken::InstallContainerWorldV1(
                      *session.Engine(), firstWorld).status);
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harken::InstallContainerWorldV1(
                      *session.Engine(), secondWorld).status);
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harken::InstallContainerWorldV1(
                      *session.Engine(), firstWorld).status);
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    RecordLocation firstRecord{};
    ASSERT_TRUE(FindRecord(
        bytes, harken::kTraceRecordCurrentWorldInstall, 0, &firstRecord));
    std::uint64_t firstRecordedResult = 0;
    std::memcpy(&firstRecordedResult,
                bytes.data() + firstRecord.payloadAt + 24,
                sizeof firstRecordedResult);
    ASSERT_NE(0u, firstRecordedResult);
    std::vector<std::uint8_t> historicalBody;
    auto historicalSecond = harkentest::AsHistoricalPrefix(secondWorld);
    historicalSecond.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(historicalSecond);
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV6(historicalSecond, historicalBody));
    ASSERT_TRUE(ReplaceCurrentWorldRecordBody(
        &bytes, 1, harken::kCurrentWorldBodyVintageV6, historicalBody));
    RecordLocation finalShippingRecord{};
    ASSERT_TRUE(FindRecord(
        bytes, harken::kTraceRecordCurrentWorldInstall, 2,
        &finalShippingRecord));
    std::memcpy(bytes.data() + finalShippingRecord.payloadAt + 16,
                &firstRecordedResult, sizeof firstRecordedResult);

    const HostRun replay = RunMutatedTrace(
        bytes, "historical_current_world_no_install.vtrc");
    EXPECT_EQ(1, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E, MalformedHistoricalCurrentWorldBodyFailsHard) {
    const std::string tracePath =
        TempPath("historical_current_world_malformed_source.vtrc");
    std::filesystem::remove(tracePath);

    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(0x9850), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.5f);
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &definition;
    world.variableCount = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);

    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harken::InstallContainerWorldV1(
                      *session.Engine(), world).status);
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    std::vector<std::uint8_t> malformedBody;
    auto historicalMalformed = harkentest::AsHistoricalPrefix(world);
    historicalMalformed.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(historicalMalformed);
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV6(historicalMalformed, malformedBody));
    ASSERT_FALSE(malformedBody.empty());
    malformedBody.pop_back();
    ASSERT_TRUE(ReplaceCurrentWorldRecordBody(
        &bytes, 0, harken::kCurrentWorldBodyVintageV6, malformedBody));

    const HostRun replay = RunMutatedTrace(
        bytes, "historical_current_world_malformed.vtrc");
    EXPECT_EQ(1, replay.exitCode) << replay.output;
}

bool EraseRecord(std::vector<std::uint8_t>* bytes,
                 const RecordLocation& record) {
    if (bytes == nullptr ||
        record.payloadAt + record.payloadBytes > bytes->size()) {
        return false;
    }
    bytes->erase(
        bytes->begin() + record.recordAt,
        bytes->begin() + record.payloadAt + record.payloadBytes);
    return true;
}


bool ReadV3Config(const std::vector<std::uint8_t>& bytes,
                  VnConfig* out, std::uint32_t* vintage) {
    if (bytes.size() < harken::kTraceFixedHeaderBytes +
                           harken::kTraceConfigWireBytes ||
        out == nullptr || vintage == nullptr) {
        return false;
    }
    std::uint32_t version = 0;
    std::uint32_t headerBytes = 0;
    std::memcpy(&version, bytes.data() + 4, sizeof version);
    std::memcpy(&headerBytes, bytes.data() + 8, sizeof headerBytes);
    return version == harken::kTraceFormatVersion &&
           headerBytes == harken::kTraceFixedHeaderBytes +
                              harken::kTraceConfigWireBytes &&
           harken::DecodeTraceConfig(
               bytes.data() + harken::kTraceFixedHeaderBytes,
               harken::kTraceConfigWireBytes, out, vintage);
}

bool WriteV3Config(std::vector<std::uint8_t>* bytes,
                   const VnConfig& config) {
    if (bytes == nullptr) return false;
    std::vector<std::uint8_t> wire;
    if (!harken::EncodeTraceConfig(config, wire) ||
        wire.size() != harken::kTraceConfigWireBytes ||
        bytes->size() < harken::kTraceFixedHeaderBytes + wire.size()) {
        return false;
    }
    std::copy(wire.begin(), wire.end(),
              bytes->begin() + harken::kTraceFixedHeaderBytes);
    const std::uint32_t vintage =
        harken::ConfigVintageFromNativeSize(config.structSize);
    std::memcpy(bytes->data() + 12, &vintage, sizeof vintage);
    return true;
}


bool RecordFormatFixture(const std::string& tracePath,
                         std::uint32_t sampleRate = 48000) {
    std::filesystem::remove(tracePath);
    EngineSession session(
        MakeNullConfig(2, kBlockFrames, sampleRate));
    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(harkentest::ObjectId(42), harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f);
    if (session.InitResult() != VN_OK ||
        harkentest::InstallDefinitionsWorld(
            session.Engine(), &definition, 1).status !=
            harken::WorldInstallStatus::Ok ||
        Vn_TraceStart(tracePath.c_str()) != VN_OK ||
        Vn_SetValue(harkentest::ObjectId(42), 0.5f) != VN_OK) {
        return false;
    }
    session.Engine()->nullBackend.Pump(1);
    if (Vn_Update() != VN_OK || Vn_TraceStop() != VN_OK) {
        return false;
    }
    return true;
}

struct CanonicalSoundActionSpec {
    VnSoundId soundId = harkentest::ObjectId(0);
    VnActionId actionId = harkentest::ObjectId(0);
    std::uint64_t contentRevision = 0;
    VnBusId route = VN_BUS_MASTER;
    bool spatialized = false;
    VnAttenuationId attenuationId = harkentest::ObjectId(0);
};

bool InstallCanonicalSoundActions(harken::Engine* engine,
                                  const CanonicalSoundActionSpec* specs,
                                  std::uint32_t count,
                                  const harken::VariableDefinitionV1* variables = nullptr,
                                  std::uint32_t variableCount = 0) {
    if (engine == nullptr || specs == nullptr || count == 0 ||
        (variables == nullptr && variableCount != 0)) {
        return false;
    }

    std::vector<harken::PlayablePropertyProgramViewV1> properties(count);
    std::vector<harken::ActionStepProgramViewV1> steps(count);
    std::vector<harken::ActionProgramViewV1> actions(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const CanonicalSoundActionSpec& spec = specs[index];
        if (spec.soundId == VnObjectId{} || spec.actionId == VnObjectId{} ||
            spec.contentRevision == 0 ||
            (index != 0 &&
             (spec.soundId <= specs[index - 1].soundId ||
              spec.actionId <= specs[index - 1].actionId))) {
            return false;
        }

        auto payload = ResolvedDefinitionPayload(spec.route);
        if (spec.spatialized) {
            payload.values[10].bits = 1;
            if (spec.attenuationId != VnObjectId{})
                payload.values[13].objectId = spec.attenuationId;
        }

        auto& property = properties[index];
        property.contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Sound;
        property.authoredStableOrder = index;
        property.stableId = spec.soundId;
        property.contentRevision = spec.contentRevision;
        property.routeStableId = spec.route;
        property.compatibilityFingerprint = index + 1u;
        property.payload = payload;
        property.programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(property);

        auto& step = steps[index];
        step.opcode = harken::ActionOpcodeV1::Play;
        step.target.kind = harken::ActionTargetKindV1::Sound;
        step.target.stableId = spec.soundId;
        step.target.expectedProgramSemanticFingerprint =
            property.programSemanticFingerprint;
        step.delay.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
        step.delay.denominator = 1;
        step.fade.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
        step.fade.denominator = 1;
        step.cooldown.domain =
            harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
        step.cooldown.denominator = 1;

        auto& action = actions[index];
        action.actionStableId = spec.actionId;
        action.steps = &step;
        action.stepCount = 1;
        action.expandedStepOccurrenceCount = 1;
        action.expandedFiringCount = 1;
        action.programSemanticFingerprint =
            harken::ComputeActionProgramFingerprintV1(action);
    }

    harken::ContainerWorldInstallViewV1 world{};
    world.variables = variables;
    world.variableCount = variableCount;
    world.playableProperties = properties.data();
    world.playablePropertyCount = count;
    world.actions = actions.data();
    world.actionCount = count;
    world.capacity.maxKnownScheduleLeaves = count;
    world.capacity.maxActionDefinitions = count;
    world.capacity.maxActionStepsPerDefinition = 1;
    world.capacity.maxActionStepsTotal = count;
    world.capacity.maxActionExpandedWork = count;
    world.identity.capabilityMask =
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityActionDefinitions |
        (variableCount == 0 ? 0
                            : harken::kWorldCapabilityVariableDefinitions);
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    return harken::InstallContainerWorldV1(*engine, world).status ==
           harken::WorldInstallStatus::Ok;
}

bool InstallCanonicalSoundAction(harken::Engine* engine,
                                 VnSoundId soundId,
                                 VnActionId actionId,
                                 std::uint64_t contentRevision,
                                 VnBusId route = VN_BUS_MASTER,
                                 bool spatialized = false,
                                 VnAttenuationId attenuationId = {}) {
    const CanonicalSoundActionSpec spec{
        soundId, actionId, contentRevision, route, spatialized, attenuationId};
    return InstallCanonicalSoundActions(engine, &spec, 1);
}

VnActionFiringHandle PostCanonicalAction(VnActionId actionId,
                                          std::uint64_t callerSeed = 0,
                                          VnEmitterHandle emitter = 0) {
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = actionId;
    desc.callerSeed = callerSeed;
    desc.emitter = emitter;
    VnActionFiringHandle firing = 0;
    return Vn_PostAction(&desc, &firing) == VN_OK ? firing : 0;
}

bool RecordActionSeekFixture(const std::string& tracePath, std::string* why) {
    const auto fail = [why](const char* message) {
        if (why != nullptr) *why = message;
        return false;
    };
    constexpr VnSoundId kSound = harkentest::ObjectId(0xA510u);
    constexpr VnActionId kAction = harkentest::ObjectId(0xA511u);
    std::filesystem::remove(tracePath);
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    if (session.InitResult() != VN_OK) return fail("init");
    harken::Engine* engine = session.Engine();
    std::vector<float> samples(4096, -0.25f);
    std::fill(samples.begin() + 1024, samples.end(), 0.5f);
    const VnPCMDesc pcm = MonoDesc(samples, 48000);
    const float* channels[1] = {samples.data()};
    std::uint64_t contentRevision = 0;
    if (!harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, samples.size(), channels, 1, &contentRevision)) {
        return fail("content revision");
    }
    if (!InstallCanonicalSoundAction(
            engine, kSound, kAction, contentRevision)) {
        return fail("install action");
    }
    if (Vn_TraceStart(tracePath.c_str()) != VN_OK) return fail("trace start");
    if (Vn_RegisterPCM(kSound, &pcm) != VN_OK) return fail("register");

    const VnActionFiringHandle firing =
        PostCanonicalAction(kAction, UINT64_C(0x1020304050607080));
    if (firing == 0) return fail("post");
    bool started = false;
    for (std::uint32_t attempt = 0; attempt < 12; ++attempt) {
        if (Vn_Update() != VN_OK) return fail("update while starting");
        engine->nullBackend.Pump(1);
        started = std::any_of(
            engine->playableServices.begin(), engine->playableServices.end(),
            [&](const harken::EnginePlayableServiceRow& row) {
                return row.phase == harken::EnginePlayableStartPhase::Started &&
                       row.InvocationValid() &&
                       row.Invocation().actionFiring == firing;
            });
        if (started) break;
    }
    if (!started || Vn_Update() != VN_OK) return fail("voice did not start");

    VnActionSeekDescV1 seek{};
    seek.structSize = sizeof(seek);
    seek.firing = firing;
    seek.positionMs = 1024.0 * 1000.0 / 48000.0;
    seek.mode = VN_ACTION_SEEK_ABSOLUTE;
    VnActionSeekTicket ticket = 0;
    if (Vn_SeekActionFiring(&seek, &ticket) != VN_OK || ticket == 0) {
        return fail("seek acceptance");
    }
    VnActionSeekInfoV1 info{};
    info.structSize = sizeof(info);
    if (Vn_QueryActionSeek(ticket, &info) != VN_OK ||
        info.state != VN_ACTION_SEEK_ACCEPTED || info.firing != firing) {
        return fail("accepted query");
    }
    engine->nullBackend.Pump(1);
    info = {};
    info.structSize = sizeof(info);
    if (Vn_QueryActionSeek(ticket, &info) != VN_OK ||
        info.state != VN_ACTION_SEEK_APPLIED || info.firing != firing ||
        info.minimumAppliedSourceFrame != 1024 ||
        info.maximumAppliedSourceFrame != 1024 ||
        Vn_ReleaseActionSeek(ticket) != VN_OK) {
        return fail("applied query or release");
    }
    return Vn_TraceStop() == VN_OK ? true : fail("trace stop");
}

bool RecordPostActionFixture(const std::string& tracePath,
                             bool rejectUnknownAction = false,
                             bool recordSecondRoot = false,
                             bool validatorSequence = false) {
    constexpr VnObjectId kActionId = harkentest::ObjectId(0xA099u);
    std::filesystem::remove(tracePath);
    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    if (session.InitResult() != VN_OK) {
        return false;
    }

    harken::VariableDefinitionV1 variable = harkentest::MakeVariableDefinition(
        harkentest::ObjectId(1), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f);
    harken::ActionStepProgramViewV1 step{};
    step.opcode = harken::ActionOpcodeV1::SetVariable;
    step.target.kind = harken::ActionTargetKindV1::Variable;
    step.target.stableId = variable.variableId;
    step.delay.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    step.delay.denominator = 1;
    step.fade.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    step.fade.denominator = 1;
    step.cooldown.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    step.cooldown.denominator = 1;
    step.valueBits = std::bit_cast<std::uint32_t>(0.5f);

    harken::ActionProgramViewV1 action{};
    action.actionStableId = harkentest::ObjectId(kActionId);
    action.steps = &step;
    action.stepCount = 1;
    action.expandedStepOccurrenceCount = 1;
    action.expandedFiringCount = 1;
    action.programSemanticFingerprint =
        harken::ComputeActionProgramFingerprintV1(action);

    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &variable;
    world.variableCount = 1;
    world.actions = &action;
    world.actionCount = 1;
    world.capacity.maxActionDefinitions = 1;
    world.capacity.maxActionStepsPerDefinition = 1;
    world.capacity.maxActionStepsTotal = 1;
    world.capacity.maxActionExpandedWork = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityActionDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    if (harken::InstallContainerWorldV1(*session.Engine(), world).status !=
            harken::WorldInstallStatus::Ok ||
        Vn_TraceStart(tracePath.c_str()) != VN_OK) {
        return false;
    }

    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = rejectUnknownAction ? harkentest::OffsetObjectId(kActionId, 1) : kActionId;
    desc.callerSeed = UINT64_C(0x1020304050607080);
    VnActionFiringHandle firing = 0;
    if (rejectUnknownAction) {
        return Vn_PostAction(&desc, &firing) == VN_UNKNOWN_ID &&
               firing == 0 && Vn_TraceStop() == VN_OK;
    }
    VnActionFiringInfo info{};
    info.structSize = sizeof(info);
    VnActionStepResult stepResult{};
    stepResult.structSize = sizeof(stepResult);
    const auto validate = [](const harken::detail::ActionAdmissionView&,
                              const void* context) noexcept -> VnResult {
        return *static_cast<const VnResult*>(context);
    };
    if (validatorSequence) {
        const VnResult refusal = VN_STALE_HANDLE;
        firing = 99;
        if (harken::detail::PostActionWithValidation(&desc, &firing, validate, &refusal) != refusal ||
            firing != 0) {
            return false;
        }
    }
    const VnResult accepted = VN_OK;
    const auto firstPost = validatorSequence
        ? harken::detail::PostActionWithValidation(&desc, &firing, validate, &accepted)
        : Vn_PostAction(&desc, &firing);
    if (firstPost != VN_OK || firing == 0) {
        return false;
    }
    VnActionFiringHandle secondFiring = 0;
    if (recordSecondRoot &&
        (Vn_Update() != VN_OK ||
         Vn_PostAction(&desc, &secondFiring) != VN_OK ||
         secondFiring == 0 || secondFiring == firing)) {
        return false;
    }
    if (recordSecondRoot && Vn_Update() != VN_OK) {
        return false;
    }
    if (Vn_StopActionFiring(firing, 37) != VN_OK ||
        Vn_QueryActionFiring(firing, &info) != VN_OK ||
        Vn_QueryActionStep(firing, 0, &stepResult) != VN_OK ||
        Vn_ReleaseActionFiring(firing) != VN_OK) {
        return false;
    }
    if (validatorSequence &&
        (Vn_QueryActionFiring(secondFiring, &info) != VN_OK ||
         Vn_QueryActionStep(secondFiring, 0, &stepResult) != VN_OK)) {
        return false;
    }
    return (!recordSecondRoot ||
            Vn_ReleaseActionFiring(secondFiring) == VN_OK) &&
           Vn_TraceStop() == VN_OK;
}

bool RecordPairedCurrentWorldTrace(const std::string& tracePath,
                                   std::string* why) {
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    std::filesystem::remove(tracePath);
    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    if (session.InitResult() != VN_OK) {
        return fail("init");
    }
    harken::Engine* engine = session.Engine();
    if (engine == nullptr) {
        return fail("engine");
    }
    std::vector<float> samples(4096, 0.25f);
    const VnPCMDesc pcm = MonoDesc(samples, 48000);
    constexpr VnSoundId kSound = harkentest::ObjectId(9101);
    constexpr VnActionId kAction = harkentest::ObjectId(91);
    constexpr VnObjectId kContainer = harkentest::ObjectId(9102);
    if (Vn_RegisterPCM(kSound, &pcm) != VN_OK) {
        return fail("register");
    }
    harken::PlayablePropertyProgramViewV1 properties[2]{};
    properties[0].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Sound;
    properties[0].stableId = kSound;
    properties[0].authoredStableOrder = 0;
    properties[0].contentRevision = engine->registry.Lookup(kSound).contentRevision;
    properties[0].routeStableId = VN_BUS_MASTER;
    properties[0].compatibilityFingerprint = 1;
    auto resolved = ResolvedDefinitionPayload();
    resolved.values[5].bits = std::bit_cast<std::uint32_t>(0.0f);
    resolved.values[7].bits = 0;
    resolved.values[8].bits = std::bit_cast<std::uint32_t>(0.0f);
    resolved.values[9].bits = std::bit_cast<std::uint32_t>(0.0f);
    resolved.values[10].bits = 0;
    properties[0].payload = resolved;
    properties[0].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[0]);
    properties[1].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Container;
    properties[1].stableId = harkentest::ObjectId(kContainer);
    properties[1].authoredStableOrder = 1;
    properties[1].contentRevision = 1;
    properties[1].routeStableId = VN_BUS_MASTER;
    properties[1].compatibilityFingerprint = 2;
    properties[1].payload = resolved;
    properties[1].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[1]);
    harken::ContainerChildProgramViewV2 child{};
    child.stableChildOrdinal = 7;
    child.authoredStableOrder = 0;
    child.playable.kind = harken::PlayableKindV1::Sound;
    child.playable.stableId = kSound;
    child.playable.expectedProgramSemanticFingerprint =
        properties[0].programSemanticFingerprint;
    child.playable.contentRevision = properties[0].contentRevision;
    child.playable.routeStableId = VN_BUS_MASTER;
    child.playable.compatibilityFingerprint = 1;
    child.frequencyWeight = 1;
    harken::PlayableOccurrencePropertyViewV1 occurrence{};
    occurrence.parentKind =
        harken::playableproperties::ContentPropertyKindV1::Container;
    occurrence.parentStableId = harkentest::ObjectId(kContainer);
    occurrence.parentProgramSemanticFingerprint =
        properties[1].programSemanticFingerprint;
    occurrence.stableOccurrenceOrdinal = child.stableChildOrdinal;
    occurrence.childPlayable = child.playable;
    occurrence.payload = EmptyOccurrencePayload();
    occurrence.payload.values[5].bits = std::bit_cast<std::uint32_t>(0.0f);
    occurrence.payload.values[8].bits = std::bit_cast<std::uint32_t>(0.0f);
    occurrence.payload.values[9].bits = std::bit_cast<std::uint32_t>(0.0f);
    occurrence.occurrenceSemanticFingerprint =
        harken::ComputePlayableOccurrenceFingerprintV1(occurrence);
    harken::ContainerProgramViewV3 container{};
    container.containerStableId = harkentest::ObjectId(kContainer);
    container.programSemanticFingerprint =
        properties[1].programSemanticFingerprint;
    container.mode = harken::ContainerModeV1::Random;
    container.randomDiscipline = harken::RandomDisciplineV1::Draw;
    container.memoryScope = harken::MemoryScopeV1::Global;
    container.childCount = 1;
    container.children = &child;
    harken::ActionStepProgramViewV1 playStep{};
    playStep.opcode = harken::ActionOpcodeV1::Play;
    playStep.target.kind = harken::ActionTargetKindV1::Sound;
    playStep.target.stableId = kSound;
    playStep.target.expectedProgramSemanticFingerprint =
        properties[0].programSemanticFingerprint;
    playStep.delay.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    playStep.delay.denominator = 1;
    playStep.fade.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    playStep.fade.denominator = 1;
    playStep.cooldown.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    playStep.cooldown.denominator = 1;
    harken::ActionProgramViewV1 action{};
    action.actionStableId = kAction;
    action.steps = &playStep;
    action.stepCount = 1;
    action.expandedStepOccurrenceCount = 1;
    action.expandedFiringCount = 1;
    action.programSemanticFingerprint =
        harken::ComputeActionProgramFingerprintV1(action);
    harken::ContainerWorldInstallViewV1 world{};
    world.playableProperties = properties;
    world.playablePropertyCount = 2;
    world.playableOccurrences = &occurrence;
    world.playableOccurrenceCount = 1;
    world.containers = &container;
    world.containerCount = 1;
    world.actions = &action;
    world.actionCount = 1;
    world.capacity.maxActionDefinitions = 4;
    world.capacity.maxActionStepsPerDefinition = 2;
    world.capacity.maxActionStepsTotal = 4;
    world.identity.capabilityMask =
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityRandomDraw |
        harken::kWorldCapabilityActionDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV5(world);
    if (harken::InstallContainerWorldV1(*engine, world).status !=
        harken::WorldInstallStatus::Ok) {
        return fail("install");
    }
    auto recorder = std::make_unique<harken::TraceRecorder>(*engine);
    if (recorder->Start(tracePath.c_str()) != VN_OK) {
        return fail("trace-start");
    }
    engine->trace = std::move(recorder);
    harken::ProjectGenerationInstallViewV1 view{};
    view.token.projectSessionId = 11;
    view.token.authoringRevision = 2;
    view.token.runtimeEpoch = 3;
    view.token.submissionSerial = 4;
    view.ownerId = 77;
    view.ownerRevision = 1;
    view.compiledWorld = world;
    harken::PreparedProjectGenerationV1 prepared;
    if (harken::PrepareProjectGenerationV1(*engine, view, prepared).status !=
        harken::ProjectGenerationPrepareStatusV1::Ok) {
        return fail("prepare");
    }
    if (harken::CommitPreparedProjectGenerationV1(*engine, prepared).status !=
        harken::ProjectGenerationCommitStatusV1::Ok) {
        return fail("commit");
    }
    if (engine->trace == nullptr) {
        return fail("trace");
    }
    engine->trace->Stop();
    engine->trace.reset();
    return true;
}

TEST(TraceReplayE2E, PairedCurrentWorldReplayRequiresAdjacentPreparedPlan) {
    const std::string tracePath = TempPath("paired_current_world_type15.vtrc");
    std::string recordWhy;
    ASSERT_TRUE(RecordPairedCurrentWorldTrace(tracePath, &recordWhy)) << recordWhy;
    const HostRun intact = RunHost("trace-replay " + tracePath);
    ASSERT_EQ(0, intact.exitCode) << intact.output;

    std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    RecordLocation plan{};
    ASSERT_TRUE(FindRecord(
        bytes, harken::kTraceRecordPreparedTopologyPlan, 0, &plan));
    RecordLocation world{};
    bool foundPairedWorld = false;
    for (std::uint32_t i = 0; i < 8; ++i) {
        RecordLocation candidate{};
        if (!FindRecord(bytes, harken::kTraceRecordCurrentWorldInstall, i,
                        &candidate)) {
            break;
        }
        if (candidate.payloadBytes >= 32u + 52u &&
            candidate.ordinal > plan.ordinal) {
            world = candidate;
            foundPairedWorld = true;
            break;
        }
    }
    ASSERT_TRUE(foundPairedWorld);
    EXPECT_LT(plan.ordinal, world.ordinal);

    std::vector<std::uint8_t> missingPlan = bytes;
    ASSERT_TRUE(EraseRecord(&missingPlan, plan));
    const HostRun missing = RunMutatedTrace(
        missingPlan, "paired_current_world_missing_type15.vtrc");
    EXPECT_EQ(1, missing.exitCode) << missing.output;

    std::vector<std::uint8_t> orphanPlan = bytes;
    ASSERT_TRUE(EraseRecord(&orphanPlan, world));
    const HostRun orphan = RunMutatedTrace(
        orphanPlan, "paired_current_world_orphan_type15.vtrc");
    EXPECT_EQ(1, orphan.exitCode) << orphan.output;

    std::vector<std::uint8_t> staleSlot = bytes;
    std::vector<RecordLocation> bump;
    ASSERT_TRUE(ScanRecords(staleSlot, &bump));
    for (const RecordLocation& record : bump) {
        if (record.ordinal >= world.ordinal) {
            const std::uint64_t bumped = record.ordinal + 2u;
            std::memcpy(staleSlot.data() + record.recordAt + 8, &bumped,
                        sizeof(bumped));
        }
    }
    const auto planBegin = staleSlot.begin() + plan.recordAt;
    const auto planEnd = staleSlot.begin() + plan.payloadAt + plan.payloadBytes;
    std::vector<std::uint8_t> duplicate(planBegin, planEnd);
    const std::uint64_t duplicateOrdinal = plan.ordinal + 1u;
    std::memcpy(duplicate.data() + 8, &duplicateOrdinal, sizeof(duplicateOrdinal));
    staleSlot.insert(staleSlot.begin() + plan.payloadAt + plan.payloadBytes,
                     duplicate.begin(), duplicate.end());
    const HostRun stale = RunMutatedTrace(
        staleSlot, "paired_current_world_stale_type15.vtrc");
    EXPECT_EQ(1, stale.exitCode) << stale.output;

    std::vector<std::uint8_t> gapped = bytes;
    std::vector<RecordLocation> gappedRecords;
    ASSERT_TRUE(ScanRecords(gapped, &gappedRecords));
    for (const RecordLocation& record : gappedRecords) {
        if (record.ordinal >= world.ordinal) {
            const std::uint64_t bumped = record.ordinal + 1u;
            std::memcpy(gapped.data() + record.recordAt + 8, &bumped,
                        sizeof(bumped));
        }
    }
    std::array<std::uint8_t, harken::kTraceRecordHeaderBytes + 8> gap{};
    const std::uint32_t gapPayload = 8;
    const std::uint16_t gapType = harken::kTraceRecordBlockBarrier;
    const std::uint16_t gapFlags = 0;
    const std::uint64_t gapOrdinal = plan.ordinal + 1u;
    const std::uint64_t gapBlock = 0;
    std::memcpy(gap.data(), &gapPayload, sizeof(gapPayload));
    std::memcpy(gap.data() + 4, &gapType, sizeof(gapType));
    std::memcpy(gap.data() + 6, &gapFlags, sizeof(gapFlags));
    std::memcpy(gap.data() + 8, &gapOrdinal, sizeof(gapOrdinal));
    std::memcpy(gap.data() + harken::kTraceRecordHeaderBytes, &gapBlock,
                sizeof(gapBlock));
    gapped.insert(gapped.begin() + world.recordAt, gap.begin(), gap.end());
    const HostRun gapRun =
        RunMutatedTrace(gapped, "paired_current_world_gapped_type15.vtrc");
    EXPECT_EQ(1, gapRun.exitCode) << gapRun.output;
}

TEST(TraceReplayE2E, ShippingCurrentWorldBodyCannotBeRetaggedAsRetiredVersions) {
    const std::string tracePath =
        TempPath("current_world_retired_shipping.vtrc");
    ASSERT_TRUE(RecordPostActionFixture(tracePath));
    auto bytes = ReadFileBytes(tracePath);
    RecordLocation install;
    ASSERT_TRUE(FindRecord(bytes, harken::kTraceRecordCurrentWorldInstall, 0,
                           &install));
    ASSERT_GE(install.payloadBytes, sizeof(std::uint32_t));
    std::uint32_t recordedVintage = 0;
    std::memcpy(&recordedVintage, bytes.data() + install.payloadAt,
                sizeof(recordedVintage));
    ASSERT_EQ(harken::kCurrentWorldShippingBodyVintage, recordedVintage);
    const HostRun currentReplay = RunHost("trace-replay " + tracePath);
    ASSERT_EQ(0, currentReplay.exitCode) << currentReplay.output;

    for (const auto vintage : {harken::kCurrentWorldBodyVintageV6,
                                harken::kCurrentWorldBodyVintageV7,
                                harken::kCurrentWorldBodyVintageV8,
                                harken::kCurrentWorldBodyVintageV9}) {
        auto retired = bytes;
        std::memcpy(retired.data() + install.payloadAt, &vintage, sizeof(vintage));
        ASSERT_NO_FATAL_FAILURE(WriteFileBytes(tracePath, retired));
        const HostRun replay = RunHost("trace-replay " + tracePath);
        EXPECT_EQ(1, replay.exitCode) << vintage << "\n" << replay.output;
    }
}

bool RecordNestedActionFiringFixture(const std::string& tracePath) {
    constexpr VnSoundId kSoundId = harkentest::ObjectId(0xA200u);
    constexpr VnActionId kChildActionId = harkentest::ObjectId(0xA201u);
    constexpr VnActionId kRootActionId = harkentest::ObjectId(0xA202u);
    std::filesystem::remove(tracePath);
    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    if (session.InitResult() != VN_OK) {
        return false;
    }
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples =
        harkentest::MakeSine(4096, 330.0, 48000.0, 0.25f);
    const float* channels[1] = {samples.data()};
    std::uint64_t contentRevision = 0;
    if (!harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, samples.size(), channels, 1, &contentRevision)) {
        return false;
    }
    VnPCMDesc pcm = MonoDesc(samples, 48000);

    const auto payload = ResolvedDefinitionPayload();
    harken::PlayablePropertyProgramViewV1 property{};
    property.contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Sound;
    property.stableId = kSoundId;
    property.contentRevision = contentRevision;
    property.routeStableId = VN_BUS_MASTER;
    property.compatibilityFingerprint = 1;
    property.payload = payload;
    property.programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(property);

    harken::ActionStepProgramViewV1 steps[2]{};
    steps[0].opcode = harken::ActionOpcodeV1::Play;
    steps[0].target.kind = harken::ActionTargetKindV1::Sound;
    steps[0].target.stableId = kSoundId;
    steps[0].target.expectedProgramSemanticFingerprint =
        property.programSemanticFingerprint;
    steps[1].opcode = harken::ActionOpcodeV1::PostAction;
    steps[1].target.kind = harken::ActionTargetKindV1::Action;
    steps[1].target.stableId = kChildActionId;
    for (harken::ActionStepProgramViewV1& step : steps) {
        step.delay.domain =
            harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
        step.delay.denominator = 1;
        step.fade.domain =
            harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
        step.fade.denominator = 1;
        step.cooldown.domain =
            harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
        step.cooldown.denominator = 1;
    }

    harken::ActionProgramViewV1 actions[2]{};
    actions[0].actionStableId = kChildActionId;
    actions[0].steps = &steps[0];
    actions[0].stepCount = 1;
    actions[0].expandedStepOccurrenceCount = 1;
    actions[0].expandedFiringCount = 1;
    actions[0].programSemanticFingerprint =
        harken::ComputeActionProgramFingerprintV1(actions[0]);
    steps[1].target.expectedProgramSemanticFingerprint =
        actions[0].programSemanticFingerprint;
    actions[1].actionStableId = kRootActionId;
    actions[1].steps = &steps[1];
    actions[1].stepCount = 1;
    actions[1].expandedStepOccurrenceCount = 2;
    actions[1].expandedFiringCount = 2;
    actions[1].programSemanticFingerprint =
        harken::ComputeActionProgramFingerprintV1(actions[1]);

    harken::ContainerWorldInstallViewV1 world{};
    world.playableProperties = &property;
    world.playablePropertyCount = 1;
    world.actions = actions;
    world.actionCount = 2;
    world.capacity.maxKnownScheduleLeaves = 1;
    world.capacity.maxActionDefinitions = 2;
    world.capacity.maxActionStepsPerDefinition = 1;
    world.capacity.maxActionStepsTotal = 2;
    world.capacity.maxActionTriggerDepth = 2;
    world.capacity.maxActionExpandedWork = 2;
    world.identity.capabilityMask =
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityActionDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    if (harken::InstallContainerWorldV1(*engine, world).status !=
            harken::WorldInstallStatus::Ok ||
        Vn_TraceStart(tracePath.c_str()) != VN_OK ||
        Vn_RegisterPCM(kSoundId, &pcm) != VN_OK) {
        return false;
    }

    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = kRootActionId;
    desc.callerSeed = UINT64_C(0x2030405060708090);
    VnActionFiringHandle rootFiring = 0;
    if (Vn_PostAction(&desc, &rootFiring) != VN_OK || rootFiring == 0) {
        return false;
    }

    VnActionFiringInfo rootInfo{};
    rootInfo.structSize = sizeof(rootInfo);
    VnActionStepResult rootStep{};
    rootStep.structSize = sizeof(rootStep);
    if (Vn_QueryActionFiring(rootFiring, &rootInfo) != VN_OK ||
        Vn_QueryActionStep(rootFiring, 0, &rootStep) != VN_OK ||
        rootStep.childFiring == 0 ||
        Vn_QueryActionStep(rootFiring, 0, &rootStep) != VN_OK) {
        return false;
    }
    const VnActionFiringHandle childFiring = rootStep.childFiring;

    VnActionFiringInfo childInfo{};
    childInfo.structSize = sizeof(childInfo);
    VnActionStepResult childStep{};
    childStep.structSize = sizeof(childStep);
    if (Vn_QueryActionFiring(childFiring, &childInfo) != VN_OK ||
        childInfo.parentFiring != rootFiring ||
        Vn_QueryActionStep(childFiring, 0, &childStep) != VN_OK ||
        childStep.producedPlayableInstance == 0 ||
        Vn_QueryActionStep(childFiring, 0, &childStep) != VN_OK) {
        return false;
    }

    VnActionStepResult invalidIndex{};
    invalidIndex.structSize = sizeof(invalidIndex);
    if (Vn_QueryActionStep(rootFiring, 9, &invalidIndex) !=
            VN_INVALID_ARG ||
        Vn_ReleaseActionFiring(childFiring) != VN_IN_USE ||
        Vn_StopActionFiring(rootFiring, 0) != VN_OK) {
        return false;
    }

    rootInfo = {};
    rootInfo.structSize = sizeof(rootInfo);
    for (std::uint32_t block = 0; block < 64; ++block) {
        if (Vn_Update() != VN_OK) {
            return false;
        }
        engine->nullBackend.Pump(1);
        if (Vn_Update() != VN_OK ||
            Vn_QueryActionFiring(rootFiring, &rootInfo) != VN_OK) {
            return false;
        }
        if (rootInfo.state == VN_ACTION_FIRING_TERMINAL_RETAINED) {
            break;
        }
    }
    if (rootInfo.state != VN_ACTION_FIRING_TERMINAL_RETAINED ||
        Vn_QueryActionStep(rootFiring, 0, &rootStep) != VN_OK ||
        Vn_QueryActionStep(childFiring, 0, &childStep) != VN_OK ||
        Vn_ReleaseActionFiring(rootFiring) != VN_OK) {
        return false;
    }

    VnActionFiringInfo staleInfo{};
    staleInfo.structSize = sizeof(staleInfo);
    if (Vn_QueryActionFiring(rootFiring, &staleInfo) != VN_STALE_HANDLE) {
        return false;
    }
    staleInfo = {};
    staleInfo.structSize = sizeof(staleInfo);
    if (Vn_QueryActionFiring(childFiring, &staleInfo) != VN_STALE_HANDLE) {
        return false;
    }
    VnActionStepResult staleStep{};
    staleStep.structSize = sizeof(staleStep);
    if (Vn_QueryActionStep(rootFiring, 0, &staleStep) != VN_STALE_HANDLE ||
        Vn_TraceStop() != VN_OK) {
        return false;
    }
    return true;
}

HostRun RunMutatedTrace(const std::vector<std::uint8_t>& bytes,
                        const std::string& suffix) {
    const std::string path = TempPath(suffix);
    WriteFileBytes(path, bytes);
    return RunHost("trace-replay " + path);
}

bool PatchApiPayload(std::vector<std::uint8_t>* bytes,
                     const ApiRecordLocation& record,
                     std::size_t fieldOffset, const void* replacement,
                     std::size_t replacementBytes) {
    if (bytes == nullptr || replacement == nullptr ||
        fieldOffset > record.payloadBytes ||
        replacementBytes > record.payloadBytes - fieldOffset ||
        record.payloadAt > bytes->size() ||
        record.payloadBytes > bytes->size() - record.payloadAt) {
        return false;
    }
    std::memcpy(bytes->data() + record.payloadAt + fieldOffset,
                replacement, replacementBytes);
    return true;
}

bool AppendApiPayloadByte(std::vector<std::uint8_t>* bytes,
                          const ApiRecordLocation& record,
                          std::uint8_t value) {
    if (bytes == nullptr ||
        record.payloadAt > bytes->size() ||
        record.payloadBytes > bytes->size() - record.payloadAt ||
        record.payloadBytes == UINT32_MAX) {
        return false;
    }
    const std::size_t payloadEnd =
        record.payloadAt + record.payloadBytes;
    bytes->insert(bytes->begin() + payloadEnd, value);
    const std::uint32_t expanded = record.payloadBytes + 1;
    std::memcpy(bytes->data() + record.recordAt, &expanded,
                sizeof expanded);
    return true;
}

bool EraseApiPayloadTail(std::vector<std::uint8_t>* bytes,
                         const ApiRecordLocation& record,
                         std::size_t count) {
    if (bytes == nullptr || count > record.payloadBytes) {
        return false;
    }
    const std::size_t payloadEnd =
        record.payloadAt + record.payloadBytes;
    if (payloadEnd > bytes->size()) {
        return false;
    }
    bytes->erase(bytes->begin() + payloadEnd - count,
                 bytes->begin() + payloadEnd);
    const std::uint32_t shortened =
        record.payloadBytes - static_cast<std::uint32_t>(count);
    std::memcpy(bytes->data() + record.recordAt, &shortened,
                sizeof shortened);
    return true;
}

bool PatchCaptureExpected(std::vector<std::uint8_t>* bytes,
                          std::uint16_t callId, std::uint32_t occurrence,
                          bool patchResult, std::uint32_t replacement) {
    ApiRecordLocation query;
    if (bytes == nullptr ||
        !FindApiRecord(*bytes, callId, occurrence, &query) ||
        query.payloadBytes < harken::kTraceCaptureResultTailBytes) {
        return false;
    }
    const std::size_t tailAt =
        query.payloadAt + query.payloadBytes -
        harken::kTraceCaptureResultTailBytes;
    const std::size_t fieldAt = tailAt + (patchResult ? 8 : 12);
    std::memcpy(bytes->data() + fieldAt, &replacement,
                sizeof replacement);
    return true;
}

// Appends the last rendered block's raw bytes to the original-session
// capture, mirroring exactly what trace-replay captures per block barrier.
void CaptureLastBlock(harken::Engine* engine,
                      std::vector<std::uint8_t>* out) {
    const float* block = engine->nullBackend.LastBlock();
    const std::size_t bytes =
        static_cast<std::size_t>(engine->nullBackend.BlockFrames()) *
        engine->nullBackend.Channels() * sizeof(float);
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(block);
    out->insert(out->end(), p, p + bytes);
}

float PeakOfRawF32(const std::vector<std::uint8_t>& bytes) {
    float peak = 0.0f;
    const std::size_t count = bytes.size() / sizeof(float);
    for (std::size_t i = 0; i < count; ++i) {
        float v = 0.0f;
        std::memcpy(&v, bytes.data() + i * sizeof(float), sizeof v);
        const float a = v < 0.0f ? -v : v;
        if (a > peak) {
            peak = a;
        }
    }
    return peak;
}

VnBindingDesc BindingDesc(VnVariableId variableId, std::uint32_t scopeKind,
                           std::uint32_t targetId, float y0, float y1,
                           std::uint32_t transitionMs) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = variableId;
    desc.targetScopeKind = scopeKind;
    desc.targetId = targetId;
    desc.transitionMs = transitionMs;
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, y0};
    desc.curve.points[1] = {1.0f, y1};
    desc.curve.handles[0] = {0.25f, y0, 0.75f, y1};
    return desc;
}

VnBindingAttachDesc AttachDesc(std::uint32_t scopeKind,
                               VnObjectId objectId) {
    VnBindingAttachDesc desc{};
    desc.structSize = sizeof(VnBindingAttachDesc);
    desc.scopeKind = scopeKind;
    desc.objectId = harkentest::ObjectId(objectId);
    return desc;
}

// Journal facts for the callback-context assertions: whether any ApiCall
// record with the given call id carries kTraceFlagCallbackContext, and
// whether the End record carries the aggregate marker.
struct CallbackFlagFacts {
    bool flaggedCallFound = false;
    bool endAggregateSet = false;
};

CallbackFlagFacts ReadCallbackFlagFacts(const std::string& path,
                                        std::uint16_t wantCallId) {
    CallbackFlagFacts facts;
    const std::vector<std::uint8_t> bytes = ReadFileBytes(path);
    if (bytes.size() < harken::kTraceFixedHeaderBytes) {
        return facts;
    }
    std::uint32_t headerBytes = 0;
    std::memcpy(&headerBytes, bytes.data() + 8, 4);
    std::size_t offset = headerBytes;
    while (offset + harken::kTraceRecordHeaderBytes <= bytes.size()) {
        std::uint32_t payloadBytes = 0;
        std::uint16_t type = 0;
        std::uint16_t flags = 0;
        std::memcpy(&payloadBytes, bytes.data() + offset, 4);
        std::memcpy(&type, bytes.data() + offset + 4, 2);
        std::memcpy(&flags, bytes.data() + offset + 6, 2);
        const std::size_t payloadAt =
            offset + harken::kTraceRecordHeaderBytes;
        if (bytes.size() - payloadAt < payloadBytes) {
            break;
        }
        if (type == harken::kTraceRecordApiCall && payloadBytes >= 2) {
            std::uint16_t callId = 0;
            std::memcpy(&callId, bytes.data() + payloadAt, 2);
            if (callId == wantCallId &&
                (flags & harken::kTraceFlagCallbackContext) != 0) {
                facts.flaggedCallFound = true;
            }
        }
        if (type == harken::kTraceRecordEnd &&
            (flags & harken::kTraceFlagCallbackContext) != 0) {
            facts.endAggregateSet = true;
        }
        offset = payloadAt + payloadBytes;
    }
    return facts;
}

// One serialized single-entry pack, enough for the hash-gate scenario.
std::vector<std::uint8_t> BuildOneEntryPack(VnSoundId id,
                                            std::size_t frames) {
    const std::vector<float> plane =
        harkentest::MakeSine(frames, 330.0, 48000.0, 0.6f);
    const std::uint64_t tocOffset = harken::kPackHeaderBytes;
    const std::uint64_t tocBytes = harken::kPackTocEntryBytes;
    const std::uint64_t align = harken::kPackBlobAlign;
    const std::uint64_t blobOffset =
        (tocOffset + tocBytes + align - 1) / align * align;
    const std::uint64_t blobBytes = plane.size() * sizeof(float);
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(blobOffset + blobBytes), 0);
    std::memcpy(bytes.data() + blobOffset, plane.data(),
                static_cast<std::size_t>(blobBytes));

    harken::PackTocEntry entry;
    entry.soundId = id;
    entry.sampleRate = 48000;
    entry.channelCount = 1;
    entry.sampleFormat = harken::kPackSampleFormatF32Planar;
    entry.frameCount = plane.size();
    entry.blobOffset = blobOffset;
    entry.blobBytes = blobBytes;
    entry.blobChecksum = harken::Fnv1a64(
        bytes.data() + blobOffset, static_cast<std::size_t>(blobBytes));
    entry.flags = 0;
    entry.loopStartFrame = 0;
    entry.loopEndFrame = 0;
    std::vector<std::uint8_t> toc(harken::kPackTocEntryBytes, 0);
    harken::SerializePackTocEntry(entry, toc.data());
    std::memcpy(bytes.data() + tocOffset, toc.data(), toc.size());

    harken::PackHeader header;
    header.formatVersion = harken::kPackFormatVersion;
    header.headerBytes = harken::kPackHeaderBytes;
    header.fileBytes = bytes.size();
    header.entryCount = 1;
    header.tocOffset = static_cast<std::uint32_t>(tocOffset);
    header.tocChecksum = harken::Fnv1a64(toc.data(), toc.size());
    harken::SerializePackHeader(header, bytes.data());
    return bytes;
}

} // namespace

TEST(TraceReplayE2E, LegacyDirectPlayAndActionCallsRemainReservedAndRejected) {
    struct ReservedCall {
        std::uint16_t callId;
        std::uint16_t expected;
    };
    constexpr std::array<ReservedCall, 10> kReservedCalls = {{
        {harken::kTraceCallReservedLegacyDirectPlay, 3},
        {harken::kTraceCallReservedLegacyActionRegister, 33},
        {harken::kTraceCallReservedLegacyActionUnregister, 34},
        {harken::kTraceCallReservedLegacyActionTrigger, 35},
        {harken::kTraceCallReservedLegacyActionFindByName, 47},
        {harken::kTraceCallReservedLegacyActionTriggerByName, 48},
        {harken::kTraceCallReservedLegacyActionReplace, 53},
        {harken::kTraceCallReservedLegacyActionTriggerEx, 87},
        {harken::kTraceCallReservedLegacyActionStop, 88},
        {harken::kTraceCallReservedLegacyActionInstanceState, 89},
    }};

    const std::string tracePath = TempPath("legacy_call_tombstones.vtrc");
    ASSERT_TRUE(RecordPostActionFixture(tracePath));
    const std::vector<std::uint8_t> source = ReadFileBytes(tracePath);
    ASSERT_FALSE(source.empty());
    ApiRecordLocation post;
    ASSERT_TRUE(FindApiRecord(source, harken::kTraceCallPostAction, 0, &post));

    for (const ReservedCall call : kReservedCalls) {
        EXPECT_EQ(call.expected, call.callId);
        std::vector<std::uint8_t> tombstone = source;
        ASSERT_TRUE(PatchApiPayload(&tombstone, post, 0, &call.callId,
                                    sizeof call.callId));
        const HostRun run = RunMutatedTrace(
            tombstone, "legacy_call_" + std::to_string(call.callId) +
                           ".vtrc");
        EXPECT_EQ(1, run.exitCode) << run.output;
    }
}

TEST(TraceReplayE2E, NativeSliceValidatorRefusalSkipsOnlyThatPost) {
    const auto path = TempPath("native_slice_validator_sequence.vtrc");
    ASSERT_TRUE(RecordPostActionFixture(path, false, true, true));
    const auto bytes = ReadFileBytes(path);
    std::array<std::uint64_t, 2> acceptedHandles{};
    for (std::uint32_t index = 0; index < 3; ++index) {
        ApiRecordLocation record;
        ASSERT_TRUE(FindApiRecord(bytes, harken::kTraceCallPostAction, index, &record));
        ASSERT_EQ((index == 0 ? 68u : 84u) + harken::kTracePostActionPairedTailBytes,
                  record.payloadBytes);
        std::uint16_t flags = 0;
        std::uint32_t result = 0;
        std::uint64_t firing = 0;
        std::memcpy(&flags, bytes.data() + record.recordAt + 6, sizeof(flags));
        std::memcpy(&result, bytes.data() + record.payloadAt + 56, sizeof(result));
        std::memcpy(&firing, bytes.data() + record.payloadAt + 60, sizeof(firing));
        if (index == 0) {
            EXPECT_NE(0u, flags & harken::kTraceFlagArgsUnreplayable);
            EXPECT_EQ(static_cast<std::uint32_t>(VN_STALE_HANDLE), result);
            EXPECT_EQ(0u, firing);
        } else {
            EXPECT_EQ(0u, flags & harken::kTraceFlagArgsUnreplayable);
            EXPECT_EQ(static_cast<std::uint32_t>(VN_OK), result);
            EXPECT_NE(0u, firing);
            acceptedHandles[index - 1] = firing;
        }
    }
    EXPECT_NE(acceptedHandles[0], acceptedHandles[1]);
    ApiRecordLocation extra;
    EXPECT_FALSE(FindApiRecord(bytes, harken::kTraceCallPostAction, 3, &extra));
    for (int replay = 0; replay < 2; ++replay) {
        const auto run = RunHost("trace-replay " + path);
        ASSERT_EQ(0, run.exitCode) << run.output;
        EXPECT_NE(std::string::npos, run.output.find("completion=complete reason=none")) << run.output;
    }
}

TEST(TraceReplayE2E, PostActionCall99ReplaysWithIndependentRootIdentity) {
    const std::string tracePath = TempPath("post_action_call_99.vtrc");
    ASSERT_TRUE(RecordPostActionFixture(tracePath));

    const HostRun first = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, first.exitCode) << first.output;
    const HostRun second = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, second.exitCode) << second.output;
}

TEST(TraceReplayE2E, ActionSeekTicketReplaysWithIndependentIdentity) {
    const std::string tracePath = TempPath("action_seek_calls_132_134.vtrc");
    std::string why;
    ASSERT_TRUE(RecordActionSeekFixture(tracePath, &why)) << why;
    const std::vector<std::uint8_t> source = ReadFileBytes(tracePath);
    ApiRecordLocation seek{};
    ApiRecordLocation queryAccepted{};
    ApiRecordLocation queryApplied{};
    ApiRecordLocation release{};
    ASSERT_TRUE(FindApiRecord(
        source, harken::kTraceCallSeekActionFiring, 0, &seek));
    ASSERT_TRUE(FindApiRecord(
        source, harken::kTraceCallQueryActionSeek, 0, &queryAccepted));
    ASSERT_TRUE(FindApiRecord(
        source, harken::kTraceCallQueryActionSeek, 1, &queryApplied));
    ASSERT_TRUE(FindApiRecord(
        source, harken::kTraceCallReleaseActionSeek, 0, &release));

    const HostRun first = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, first.exitCode) << first.output;
    const HostRun second = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, second.exitCode) << second.output;
}

TEST(TraceReplayE2E,
     ActionFiringTreeAndPlayableObservationsReplayDeterministicallyTwice) {
    const std::string tracePath =
        TempPath("action_firing_tree_calls_99_103.vtrc");
    ASSERT_TRUE(RecordNestedActionFiringFixture(tracePath));

    const HostRun first = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, first.exitCode) << first.output;
    const HostRun second = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, second.exitCode) << second.output;
}

TEST(TraceReplayE2E, PostActionRefusalWithZeroRootReplaysTwice) {
    const std::string tracePath =
        TempPath("post_action_refusal_call_99.vtrc");
    ASSERT_TRUE(RecordPostActionFixture(tracePath, true));

    const HostRun first = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, first.exitCode) << first.output;
    const HostRun second = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, second.exitCode) << second.output;
}

TEST(TraceReplayE2E,
     ActionFiringCallsRejectMalformedPayloadsAndRootIdentityConflicts) {
    const std::string tracePath =
        TempPath("action_firing_mutation_source.vtrc");
    ASSERT_TRUE(RecordPostActionFixture(tracePath, false, true));
    const HostRun baseline = RunHost("trace-replay " + tracePath);
    ASSERT_EQ(0, baseline.exitCode) << baseline.output;
    const std::vector<std::uint8_t> source = ReadFileBytes(tracePath);
    ASSERT_FALSE(source.empty());

    struct CallField {
        std::uint16_t callId;
        std::size_t offset;
    };
    constexpr std::array<std::uint16_t, 5> kCallIds = {
        harken::kTraceCallPostAction,
        harken::kTraceCallStopActionFiring,
        harken::kTraceCallQueryActionFiring,
        harken::kTraceCallQueryActionStep,
        harken::kTraceCallReleaseActionFiring};
    constexpr std::array<CallField, 5> kResultFields = {{
        {harken::kTraceCallPostAction, 56},
        {harken::kTraceCallStopActionFiring, 20},
        {harken::kTraceCallQueryActionFiring, 20},
        {harken::kTraceCallQueryActionStep, 24},
        {harken::kTraceCallReleaseActionFiring, 16},
    }};
    constexpr std::array<CallField, 5> kSizeFields = {{
        {harken::kTraceCallPostAction, 8},
        {harken::kTraceCallQueryActionFiring, 16},
        {harken::kTraceCallQueryActionFiring, 24},
        {harken::kTraceCallQueryActionStep, 20},
        {harken::kTraceCallQueryActionStep, 28},
    }};
    constexpr std::array<CallField, 6> kEnumFields = {{
        {harken::kTraceCallQueryActionFiring, 28},
        {harken::kTraceCallQueryActionFiring, 32},
        {harken::kTraceCallQueryActionFiring, 36},
        {harken::kTraceCallQueryActionStep, 32},
        {harken::kTraceCallQueryActionStep, 40},
        {harken::kTraceCallQueryActionStep, 112},
    }};
    const auto expectMalformed = [&](const std::vector<std::uint8_t>& bytes,
                                     const std::string& name) {
        const HostRun run = RunMutatedTrace(bytes, name);
        EXPECT_EQ(1, run.exitCode) << run.output;
    };

    for (const std::uint16_t callId : kCallIds) {
        ApiRecordLocation record;
        ASSERT_TRUE(FindApiRecord(source, callId, 0, &record));

        std::vector<std::uint8_t> schema = source;
        const std::uint32_t unknownSchema = UINT32_MAX;
        ASSERT_TRUE(PatchApiPayload(&schema, record, 4, &unknownSchema,
                                    sizeof unknownSchema));
        expectMalformed(schema, "action_firing_schema_" +
                                    std::to_string(callId) + ".vtrc");

        std::vector<std::uint8_t> reserved = source;
        const std::uint16_t nonZeroReserved = 1;
        ASSERT_TRUE(PatchApiPayload(&reserved, record, 2,
                                    &nonZeroReserved,
                                    sizeof nonZeroReserved));
        expectMalformed(reserved, "action_firing_reserved_" +
                                      std::to_string(callId) + ".vtrc");

        std::vector<std::uint8_t> truncated = source;
        ASSERT_TRUE(EraseApiPayloadTail(&truncated, record, 1));
        expectMalformed(truncated, "action_firing_truncated_" +
                                       std::to_string(callId) + ".vtrc");

        std::vector<std::uint8_t> trailing = source;
        ASSERT_TRUE(AppendApiPayloadByte(&trailing, record, 0x5A));
        expectMalformed(trailing, "action_firing_trailing_" +
                                      std::to_string(callId) + ".vtrc");
    }

    for (const CallField field : kResultFields) {
        ApiRecordLocation record;
        ASSERT_TRUE(FindApiRecord(source, field.callId, 0, &record));
        std::vector<std::uint8_t> bytes = source;
        const std::uint32_t mismatchedResult =
            static_cast<std::uint32_t>(VN_INVALID_ARG);
        ASSERT_TRUE(PatchApiPayload(&bytes, record, field.offset,
                                    &mismatchedResult,
                                    sizeof mismatchedResult));
        expectMalformed(bytes, "action_firing_result_" +
                                   std::to_string(field.callId) + ".vtrc");
    }

    for (std::size_t index = 0; index < kSizeFields.size(); ++index) {
        const CallField field = kSizeFields[index];
        ApiRecordLocation record;
        ASSERT_TRUE(FindApiRecord(source, field.callId, 0, &record));
        std::vector<std::uint8_t> bytes = source;
        const std::uint32_t invalidSize = 0;
        ASSERT_TRUE(PatchApiPayload(&bytes, record, field.offset,
                                    &invalidSize, sizeof invalidSize));
        expectMalformed(bytes, "action_firing_size_" +
                                   std::to_string(index) + ".vtrc");
    }

    for (std::size_t index = 0; index < kEnumFields.size(); ++index) {
        const CallField field = kEnumFields[index];
        ApiRecordLocation record;
        ASSERT_TRUE(FindApiRecord(source, field.callId, 0, &record));
        std::vector<std::uint8_t> bytes = source;
        const std::uint32_t invalidEnum = UINT32_MAX;
        ASSERT_TRUE(PatchApiPayload(&bytes, record, field.offset,
                                    &invalidEnum, sizeof invalidEnum));
        expectMalformed(bytes, "action_firing_enum_" +
                                   std::to_string(index) + ".vtrc");
    }

    ApiRecordLocation stop;
    ASSERT_TRUE(FindApiRecord(
        source, harken::kTraceCallStopActionFiring, 0, &stop));
    std::vector<std::uint8_t> unmapped = source;
    const std::uint64_t unknownFiring = UINT64_MAX;
    ASSERT_TRUE(PatchApiPayload(&unmapped, stop, 8, &unknownFiring,
                                sizeof unknownFiring));
    expectMalformed(unmapped, "action_firing_unmapped_input.vtrc");

    ApiRecordLocation firstPost;
    ApiRecordLocation secondPost;
    ASSERT_TRUE(FindApiRecord(source, harken::kTraceCallPostAction, 0,
                              &firstPost));
    ASSERT_TRUE(FindApiRecord(source, harken::kTraceCallPostAction, 1,
                              &secondPost));
    std::uint64_t firstRecordedFiring = 0;
    std::memcpy(&firstRecordedFiring,
                source.data() + firstPost.payloadAt + 60,
                sizeof firstRecordedFiring);
    ASSERT_NE(0u, firstRecordedFiring);
    std::vector<std::uint8_t> duplicateRoot = source;
    ASSERT_TRUE(PatchApiPayload(&duplicateRoot, secondPost, 60,
                                &firstRecordedFiring,
                                sizeof firstRecordedFiring));
    expectMalformed(duplicateRoot,
                    "action_firing_duplicate_root_identity.vtrc");
}

TEST(TraceReplayE2E,
     ActionFiringCallsRejectChildAndPlayableIdentityConflicts) {
    const std::string tracePath =
        TempPath("action_firing_identity_conflict_source.vtrc");
    ASSERT_TRUE(RecordNestedActionFiringFixture(tracePath));
    const std::vector<std::uint8_t> source = ReadFileBytes(tracePath);
    ASSERT_FALSE(source.empty());
    const auto expectMalformed = [&](const std::vector<std::uint8_t>& bytes,
                                     const std::string& name) {
        const HostRun run = RunMutatedTrace(bytes, name);
        EXPECT_EQ(1, run.exitCode) << run.output;
    };

    ApiRecordLocation repeatedRootStep;
    ASSERT_TRUE(FindApiRecord(
        source, harken::kTraceCallQueryActionStep, 1, &repeatedRootStep));
    std::vector<std::uint8_t> childConflict = source;
    const std::uint64_t conflictingChild = UINT64_C(0xE100000000000001);
    ASSERT_TRUE(PatchApiPayload(&childConflict, repeatedRootStep, 68,
                                &conflictingChild,
                                sizeof conflictingChild));
    expectMalformed(childConflict,
                    "action_firing_child_identity_conflict.vtrc");

    ApiRecordLocation repeatedChildStep;
    ASSERT_TRUE(FindApiRecord(
        source, harken::kTraceCallQueryActionStep, 3, &repeatedChildStep));
    std::vector<std::uint8_t> playableConflict = source;
    const std::uint64_t conflictingPlayable =
        UINT64_C(0xE200000000000001);
    ASSERT_TRUE(PatchApiPayload(&playableConflict, repeatedChildStep, 76,
                                &conflictingPlayable,
                                sizeof conflictingPlayable));
    expectMalformed(playableConflict,
                    "action_firing_playable_identity_conflict.vtrc");
}

TEST(TraceReplayE2E, DoubleReplayIsByteIdenticalToItselfAndTheSession) {
    constexpr VnBusId kRoutedBus = harkentest::ObjectId(7);
    const std::string tracePath = TempPath("session.vtrc");
    const std::string out1 = TempPath("replay1.f32");
    const std::string out2 = TempPath("replay2.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(out1);
    std::filesystem::remove(out2);

    std::vector<std::uint8_t> original;
    {
        VnConfig config = MakeNullConfig(16, kBlockFrames);
        config.randomSeed = 20260711u;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const harken::VariableDefinitionV1 definition =
            harkentest::MakeVariableDefinition(harkentest::ObjectId(5), harken::VariableKindV1::Smooth,
                                             0.0f, 1.0f, 0.0f);

        const std::vector<float> toneA =
            harkentest::MakeSine(8000, 220.0, 48000.0, 0.5f);
        const std::vector<float> toneB =
            harkentest::MakeSine(6000, 523.0, 48000.0, 0.4f);
        VnPCMDesc pcmA = MonoDesc(toneA, 48000);
        VnPCMDesc pcmB = MonoDesc(toneB, 48000);
        const float* toneAChannels[1] = {toneA.data()};
        const float* toneBChannels[1] = {toneB.data()};
        std::uint64_t toneARevision = 0;
        std::uint64_t toneBRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, toneA.size(), toneAChannels, 1, &toneARevision));
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, toneB.size(), toneBChannels, 1, &toneBRevision));

        const auto payload = ResolvedDefinitionPayload(kRoutedBus);
        harken::PlayablePropertyProgramViewV1 properties[2]{};
        for (std::uint32_t index = 0; index < 2; ++index) {
            properties[index].contentPropertyKind =
                harken::playableproperties::ContentPropertyKindV1::Sound;
            properties[index].authoredStableOrder = index;
            properties[index].stableId = harkentest::ObjectId(index + 1u);
            properties[index].contentRevision =
                index == 0 ? toneARevision : toneBRevision;
            properties[index].routeStableId = kRoutedBus;
            properties[index].compatibilityFingerprint = index + 1u;
            properties[index].payload = payload;
            properties[index].programSemanticFingerprint =
                harken::ComputePlayablePropertyFingerprintV1(
                    properties[index]);
        }

        auto setImmediateTime = [](harken::AuthoredTimeRationalV1* time) {
            time->domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
            time->denominator = 1;
        };
        harken::ActionStepProgramViewV1 steps[3]{};
        steps[0].opcode = harken::ActionOpcodeV1::Play;
        steps[0].target.kind = harken::ActionTargetKindV1::Sound;
        steps[0].target.stableId = properties[1].stableId;
        steps[0].target.expectedProgramSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        setImmediateTime(&steps[0].delay);
        setImmediateTime(&steps[0].fade);
        setImmediateTime(&steps[0].cooldown);

        harken::ActionProgramViewV1 actions[2]{};
        actions[0].actionStableId = harkentest::ObjectId(10);
        actions[0].steps = &steps[0];
        actions[0].stepCount = 1;
        actions[0].expandedStepOccurrenceCount = 1;
        actions[0].expandedFiringCount = 1;
        actions[0].programSemanticFingerprint =
            harken::ComputeActionProgramFingerprintV1(actions[0]);

        steps[1].opcode = harken::ActionOpcodeV1::Play;
        steps[1].target.kind = harken::ActionTargetKindV1::Sound;
        steps[1].target.stableId = properties[0].stableId;
        steps[1].target.expectedProgramSemanticFingerprint =
            properties[0].programSemanticFingerprint;
        setImmediateTime(&steps[1].delay);
        setImmediateTime(&steps[1].fade);
        setImmediateTime(&steps[1].cooldown);
        steps[2].opcode = harken::ActionOpcodeV1::PostAction;
        steps[2].authoredEventIndex = 1;
        steps[2].target.kind = harken::ActionTargetKindV1::Action;
        steps[2].target.stableId = actions[0].actionStableId;
        steps[2].target.expectedProgramSemanticFingerprint =
            actions[0].programSemanticFingerprint;
        setImmediateTime(&steps[2].delay);
        setImmediateTime(&steps[2].fade);
        setImmediateTime(&steps[2].cooldown);
        actions[1].actionStableId = harkentest::ObjectId(11);
        actions[1].steps = &steps[1];
        actions[1].stepCount = 2;
        actions[1].expandedStepOccurrenceCount = 3;
        actions[1].expandedFiringCount = 2;
        actions[1].programSemanticFingerprint =
            harken::ComputeActionProgramFingerprintV1(actions[1]);

        harken::ContainerWorldInstallViewV1 world{};
        world.variables = &definition;
        world.variableCount = 1;
        world.playableProperties = properties;
        world.playablePropertyCount = 2;
        world.actions = actions;
        world.actionCount = 2;
        world.capacity.maxKnownScheduleLeaves = 2;
        world.capacity.maxActionDefinitions = 2;
        world.capacity.maxActionStepsPerDefinition = 2;
        world.capacity.maxActionStepsTotal = 3;
        world.capacity.maxActionTriggerDepth = 2;
        world.capacity.maxActionExpandedWork = 3;
        world.identity.capabilityMask =
            harken::kWorldCapabilityVariableDefinitions |
            harken::kWorldCapabilityPlayableProperties |
            harken::kWorldCapabilityActionDefinitions;
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harken::InstallContainerWorldV1(*engine, world).status);

        // Blocks rendered before the trace begins exercise the journal's
        // base-block accounting; they are outside the captured window.
        for (std::uint32_t block = 0; block < 2; ++block) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
        }
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));

        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcmA));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &pcmB));

        ASSERT_EQ(VN_OK, Vn_CreateBus(kRoutedBus, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(kRoutedBus, -3.0f, 100));
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(8), VN_BUS_MASTER));

        VnPostActionDesc actionDesc{};
        actionDesc.structSize = sizeof(actionDesc);
        actionDesc.actionId = actions[1].actionStableId;
        actionDesc.callerSeed = UINT64_C(0x3141592653589793);
        VnActionFiringHandle actionFiring = 0;
        ASSERT_EQ(VN_OK, Vn_PostAction(&actionDesc, &actionFiring));
        ASSERT_NE(0u, actionFiring);

        // Every block is pumped singly and captured immediately, keeping
        // this capture 1:1 with the journal's block barriers (which is
        // exactly how trace-replay captures on the other side).
        for (int i = 0; i < 30; ++i) {
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
            if (i % 2 == 0) {
                ASSERT_EQ(VN_OK, Vn_Update());
            }
            if (i == 6) {
                ASSERT_EQ(VN_OK, Vn_SetBusMute(kRoutedBus, 1, 10));
            }
            if (i == 10) {
                ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(5), 0.5f));
            }
            if (i == 12) {
                ASSERT_EQ(VN_OK, Vn_SetBusMute(kRoutedBus, 0, 5));
            }
            if (i == 16) {
                ASSERT_EQ(VN_OK, Vn_SetBusSolo(harkentest::ObjectId(8), 1, 0));
            }
            if (i == 18) {
                ASSERT_EQ(VN_OK, Vn_ReparentBus(kRoutedBus, harkentest::ObjectId(8)));
            }
            if (i == 20) {
                ASSERT_EQ(VN_OK, Vn_SetBusSolo(harkentest::ObjectId(8), 0, 0));
                ASSERT_EQ(VN_OK,
                          Vn_ReparentBus(kRoutedBus, VN_BUS_MASTER));
            }
            if (i == 24) {
                ASSERT_EQ(VN_OK, Vn_DestroyBus(harkentest::ObjectId(8)));
            }
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        for (int i = 0; i < 2; ++i) {
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
        // Session ends here; EngineSession's destructor terms the engine.
    }
    // Silence guard: a byte comparison between silent renders proves
    // nothing, so the recorded session itself must be audible.
    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);

    // Replay twice; both captures must match each other AND the original
    // session byte for byte.
    const HostRun run1 =
        RunHost("trace-replay " + tracePath + " --capture " + out1);
    ASSERT_EQ(0, run1.exitCode) << run1.output;
    const HostRun run2 =
        RunHost("trace-replay " + tracePath + " --null --capture " + out2);
    ASSERT_EQ(0, run2.exitCode) << run2.output;

    const std::vector<std::uint8_t> replay1 = ReadFileBytes(out1);
    const std::vector<std::uint8_t> replay2 = ReadFileBytes(out2);
    ASSERT_FALSE(replay1.empty());
    ASSERT_GT(PeakOfRawF32(replay1), harkentest::kSilenceFloor);
    EXPECT_EQ(replay1, replay2) << "two replays of one journal diverged";
    EXPECT_EQ(original.size(), replay1.size());
    EXPECT_TRUE(original == replay1)
        << "replay diverged from the recorded session's render";
}

TEST(TraceReplayE2E, VersionOneTraceIsExplicitlyRejected) {
    const std::string tracePath = TempPath("version_one.vtrc");
    const std::string outPath = TempPath("version_one.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(outPath);
    {
        EngineSession session(MakeNullConfig(2, kBlockFrames));
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    ASSERT_GE(bytes.size(), 8u);
    const std::uint32_t versionOne = 1;
    std::memcpy(bytes.data() + 4, &versionOne, sizeof versionOne);
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(tracePath, bytes));

    const HostRun run =
        RunHost("trace-replay " + tracePath + " --capture " + outPath);
    EXPECT_EQ(1, run.exitCode) << run.output;
    EXPECT_FALSE(std::filesystem::exists(outPath));
}

TEST(TraceReplayE2E, RetiredTraceVersionsAreRejectedBeforeRecords) {
    const auto tracePath = TempPath("retired_versions_source.vtrc");
    ASSERT_TRUE(RecordFormatFixture(tracePath));
    const auto source = ReadFileBytes(tracePath);
    ASSERT_GE(source.size(), 8u);
    for (std::uint32_t version = 1; version < harken::kTraceFormatVersion; ++version) {
        auto bytes = source;
        std::memcpy(bytes.data() + 4, &version, sizeof(version));
        const auto run = RunMutatedTrace(
            bytes, "retired_version_" + std::to_string(version) + ".vtrc");
        EXPECT_EQ(1, run.exitCode) << run.output;
    }
}

TEST(TraceReplayE2E, V2OrdinalsAreStrictlyIncreasingButMayHaveGaps) {
    const std::string tracePath = TempPath("format_ordinal_source.vtrc");
    ASSERT_TRUE(RecordFormatFixture(tracePath));
    const std::vector<std::uint8_t> source = ReadFileBytes(tracePath);
    std::vector<RecordLocation> records;
    ASSERT_TRUE(ScanRecords(source, &records));
    ASSERT_GE(records.size(), 3u);

    std::vector<std::uint8_t> duplicate = source;
    ASSERT_TRUE(PatchRecordOrdinal(
        &duplicate, records[1], records[0].ordinal));
    const HostRun duplicateRun =
        RunMutatedTrace(duplicate, "format_ordinal_duplicate.vtrc");
    EXPECT_EQ(1, duplicateRun.exitCode) << duplicateRun.output;

    std::vector<std::uint8_t> decreasing = source;
    ASSERT_TRUE(PatchRecordOrdinal(
        &decreasing, records[2], records[0].ordinal));
    const HostRun decreasingRun =
        RunMutatedTrace(decreasing, "format_ordinal_decreasing.vtrc");
    EXPECT_EQ(1, decreasingRun.exitCode) << decreasingRun.output;

    std::vector<std::uint8_t> gap = source;
    for (std::size_t i = 1; i < records.size(); ++i) {
        ASSERT_TRUE(PatchRecordOrdinal(
            &gap, records[i], records[i].ordinal + 100));
    }
    const HostRun gapRun =
        RunMutatedTrace(gap, "format_ordinal_gap.vtrc");
    EXPECT_EQ(0, gapRun.exitCode) << gapRun.output;
}

TEST(TraceReplayE2E, V2RecordPayloadShapesAreExact) {
    const std::string tracePath = TempPath("format_payload_source.vtrc");
    ASSERT_TRUE(RecordFormatFixture(tracePath));
    const std::vector<std::uint8_t> source = ReadFileBytes(tracePath);

    const std::array<std::uint16_t, 4> types = {
        harken::kTraceRecordBlockBarrier,
        harken::kTraceRecordUpdateBegin,
        harken::kTraceRecordUpdateEnd,
        harken::kTraceRecordEnd,
    };
    for (const std::uint16_t type : types) {
        RecordLocation record;
        ASSERT_TRUE(FindRecord(source, type, 0, &record));
        const std::array<std::uint32_t, 2> sizes =
            type == harken::kTraceRecordEnd
                ? std::array<std::uint32_t, 2>{1u, 2u}
                : std::array<std::uint32_t, 2>{7u, 9u};
        for (const std::uint32_t size : sizes) {
            std::vector<std::uint8_t> bytes = source;
            ASSERT_TRUE(ResizeRecordPayload(&bytes, record, size));
            const HostRun run = RunMutatedTrace(
                bytes, "format_payload_" + std::to_string(type) +
                           "_" + std::to_string(size) + ".vtrc");
            EXPECT_EQ(1, run.exitCode) << run.output;
        }
    }

    ApiRecordLocation api;
    ASSERT_TRUE(FindApiRecord(
        source, harken::kTraceCallSetValue, 0, &api));
    RecordLocation apiRecord{
        api.recordAt,
        api.payloadAt,
        api.payloadBytes,
        harken::kTraceRecordApiCall,
        0,
    };
    std::vector<std::uint8_t> trailingCallByte = source;
    ASSERT_TRUE(ResizeRecordPayload(
        &trailingCallByte, apiRecord, api.payloadBytes + 1));
    const HostRun callRun = RunMutatedTrace(
        trailingCallByte, "format_call_trailing_byte.vtrc");
    EXPECT_EQ(1, callRun.exitCode) << callRun.output;

    for (const std::uint32_t size : {0u, 2u}) {
        std::vector<std::uint8_t> shortUnreplayable = source;
        ASSERT_TRUE(ResizeRecordPayload(
            &shortUnreplayable, apiRecord, size));
        ASSERT_TRUE(PatchRecordFlags(
            &shortUnreplayable, apiRecord,
            harken::kTraceFlagArgsUnreplayable));
        const HostRun run = RunMutatedTrace(
            shortUnreplayable,
            "format_unreplayable_short_" +
                std::to_string(size) + ".vtrc");
        EXPECT_EQ(1, run.exitCode) << run.output;
    }
}

TEST(TraceReplayE2E, V2BarrierIndicesAndTerminalEndAreStrict) {
    const std::string tracePath = TempPath("format_barrier_source.vtrc");
    ASSERT_TRUE(RecordFormatFixture(tracePath));
    const std::vector<std::uint8_t> source = ReadFileBytes(tracePath);

    RecordLocation block;
    ASSERT_TRUE(FindRecord(
        source, harken::kTraceRecordBlockBarrier, 0, &block));
    std::uint64_t blockIndex = 0;
    std::memcpy(&blockIndex, source.data() + block.payloadAt,
                sizeof blockIndex);
    std::vector<std::uint8_t> wrongBlock = source;
    ASSERT_TRUE(PatchRecordPayloadU64(
        &wrongBlock, block, blockIndex + 1));
    const HostRun blockRun =
        RunMutatedTrace(wrongBlock, "format_block_index.vtrc");
    EXPECT_EQ(1, blockRun.exitCode) << blockRun.output;

    RecordLocation updateEnd;
    ASSERT_TRUE(FindRecord(
        source, harken::kTraceRecordUpdateEnd, 0, &updateEnd));
    std::vector<std::uint8_t> wrongUpdate = source;
    ASSERT_TRUE(PatchRecordPayloadU64(
        &wrongUpdate, updateEnd, 1));
    const HostRun updateRun =
        RunMutatedTrace(wrongUpdate, "format_update_index.vtrc");
    EXPECT_EQ(1, updateRun.exitCode) << updateRun.output;

    std::vector<std::uint8_t> trailing = source;
    trailing.push_back(0x5A);
    const HostRun trailingRun =
        RunMutatedTrace(trailing, "format_end_trailing.vtrc");
    EXPECT_EQ(1, trailingRun.exitCode) << trailingRun.output;
}

TEST(TraceReplayE2E, ReportsCompleteAndPartialCompletionPrecisely) {
    const std::string tracePath = TempPath("format_completion_source.vtrc");
    ASSERT_TRUE(RecordFormatFixture(tracePath));
    const std::vector<std::uint8_t> source = ReadFileBytes(tracePath);

    const HostRun complete =
        RunMutatedTrace(source, "format_completion_complete.vtrc");
    ASSERT_EQ(0, complete.exitCode) << complete.output;
    EXPECT_NE(std::string::npos,
              complete.output.find(
                  "completion=complete reason=none"))
        << complete.output;
    EXPECT_NE(std::string::npos,
              complete.output.find(
                  "device_format_identity=exact "
                  "rate_identity=exact "
                  "device_layout_identity=exact"))
        << complete.output;

    RecordLocation end;
    ASSERT_TRUE(FindRecord(source, harken::kTraceRecordEnd, 0, &end));
    std::vector<std::uint8_t> missingEnd = source;
    ASSERT_TRUE(EraseRecord(&missingEnd, end));
    const HostRun missing =
        RunMutatedTrace(missingEnd, "format_completion_missing.vtrc");
    ASSERT_EQ(0, missing.exitCode) << missing.output;
    EXPECT_NE(std::string::npos,
              missing.output.find(
                  "completion=partial reason=missing-end"))
        << missing.output;

    std::vector<std::uint8_t> truncated = source;
    ASSERT_FALSE(truncated.empty());
    truncated.pop_back();
    const HostRun truncatedRun =
        RunMutatedTrace(truncated, "format_completion_truncated.vtrc");
    ASSERT_EQ(0, truncatedRun.exitCode) << truncatedRun.output;
    EXPECT_NE(std::string::npos,
              truncatedRun.output.find(
                  "completion=partial reason=truncated-record"))
        << truncatedRun.output;
}

TEST(TraceReplayE2E,
     Explicit44100NullTraceReplaysWithExactFormatIdentity) {
    constexpr VnSoundId kSoundId = harkentest::ObjectId(9101);
    constexpr VnActionId kActionId = harkentest::ObjectId(9102);
    const std::string tracePath = TempPath("rate_44100.vtrc");
    const std::string capturePath = TempPath("rate_44100.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(capturePath);

    std::vector<std::uint8_t> original;
    {
        EngineSession session(
            MakeNullConfig(2, kBlockFrames, 44100));
        ASSERT_EQ(VN_OK, session.InitResult());
        const std::vector<float> tone =
            harkentest::MakeSine(1024, 440.0, 44100.0, 0.25f);
        const VnPCMDesc pcm = MonoDesc(tone, 44100);
        const float* channels[1] = {tone.data()};
        std::uint64_t contentRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            44100, 1, tone.size(), channels, 1, &contentRevision));
        ASSERT_TRUE(InstallCanonicalSoundAction(
            session.Engine(), kSoundId, kActionId, contentRevision));
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSoundId, &pcm));
        ASSERT_NE(0u, PostCanonicalAction(kActionId));
        for (int i = 0; i < 12; ++i) {
            ASSERT_EQ(VN_OK, Vn_Update());
            session.Engine()->nullBackend.Pump(1);
            CaptureLastBlock(session.Engine(), &original);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);
    const HostRun run = RunHost(
        "trace-replay " + tracePath + " --capture " + capturePath);
    ASSERT_EQ(0, run.exitCode) << run.output;
    EXPECT_NE(std::string::npos,
              run.output.find(
                  "device_format_identity=exact "
                  "rate_identity=exact "
                  "device_layout_identity=exact "
                  "replay_rate_hz=44100"))
        << run.output;
    const std::vector<std::uint8_t> replay =
        ReadFileBytes(capturePath);
    ASSERT_GT(PeakOfRawF32(replay), harkentest::kSilenceFloor);
    EXPECT_EQ(original, replay);
}

TEST(TraceReplayE2E,
     RealDeviceDefaultFormatTraceUsesResolvedV3Provenance) {
    const std::string tracePath =
        TempPath("real_default_format_source.vtrc");
    ASSERT_TRUE(RecordFormatFixture(tracePath, 0));
    std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    VnConfig recordedConfig{};
    std::uint32_t recordedVintage = 0;
    ASSERT_TRUE(ReadV3Config(bytes, &recordedConfig, &recordedVintage));
    recordedConfig.deviceMode = VN_DEVICE_REAL;
    ASSERT_TRUE(WriteV3Config(&bytes, recordedConfig));
    const HostRun run = RunMutatedTrace(
        bytes, "real_default_format_replay.vtrc");
    ASSERT_EQ(0, run.exitCode) << run.output;
    EXPECT_NE(std::string::npos,
              run.output.find(
                  "device_format_identity=exact "
                  "rate_identity=exact "
                  "device_layout_identity=exact "
                  "replay_rate_hz=48000 replay_device_layout=2"))
        << run.output;
    EXPECT_EQ(std::string::npos,
              run.output.find("not proof of device-format identity"))
        << run.output;

    RecordLocation end;
    ASSERT_TRUE(FindRecord(
        bytes, harken::kTraceRecordEnd, 0, &end));
    ASSERT_TRUE(EraseRecord(&bytes, end));
    const HostRun partial = RunMutatedTrace(
        bytes, "real_default_format_partial.vtrc");
    ASSERT_EQ(0, partial.exitCode) << partial.output;
    EXPECT_NE(std::string::npos,
              partial.output.find(
                  "completion=partial reason=missing-end"))
        << partial.output;
    EXPECT_NE(std::string::npos,
              partial.output.find(
                  "device_format_identity=exact"))
        << partial.output;
}


TEST(TraceReplayE2E,
     BusTopologySnapshotRetriesAndConflictsReplayDeterministically) {
    const std::string tracePath = TempPath("bus_topology.vtrc");
    std::filesystem::remove(tracePath);
    {
        VnConfig config = MakeNullConfig(2, kBlockFrames);
        config.maxBuses = 4;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));

        std::array buses{
            harken::BusTopologyBuilder::Bus(harkentest::ObjectId(8201), VN_BUS_MASTER)};
        VnBusTopologyDesc topology =
            harken::BusTopologyBuilder::Snapshot(
                0x5452414345425553ull, 0, 1, buses.data(),
                static_cast<std::uint32_t>(buses.size()));
        ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&topology));
        ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&topology));

        std::array conflictRows{
            harken::BusTopologyBuilder::Bus(harkentest::ObjectId(8202), VN_BUS_MASTER)};
        const VnBusTopologyDesc conflict =
            harken::BusTopologyBuilder::Snapshot(
                topology.ownerId, 0, 1, conflictRows.data(),
                static_cast<std::uint32_t>(conflictRows.size()));
        ASSERT_EQ(VN_REVISION_CONFLICT, Vn_ApplyBusTopology(&conflict));

        session.Engine()->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const std::vector<std::uint8_t> bytes = ReadFileBytes(tracePath);
    ApiRecordLocation apply;
    ASSERT_TRUE(FindApiRecord(
        bytes, harken::kTraceCallApplyBusTopology, 0, &apply));
    const HostRun replay = RunHost("trace-replay " + tracePath);
    EXPECT_EQ(0, replay.exitCode) << replay.output;
}

TEST(TraceReplayE2E, CallbackContextCallIsFlaggedAndReplayWarnsNotSilently) {
    constexpr VnSoundId kSound = harkentest::ObjectId(1);
    constexpr VnActionId kAction = harkentest::ObjectId(2);
    constexpr VnSoundId kCallbackSound = harkentest::ObjectId(3);
    const std::string tracePath = TempPath("callback_session.vtrc");
    const std::string outPath = TempPath("callback_replay.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(outPath);

    {
        VnConfig config = MakeNullConfig(8, kBlockFrames);
        config.randomSeed = 3u;
        // A notification callback that re-enters the API with a stateful
        // call: the recorder journals it inside the enclosing Vn_Update's
        // barrier pair and must flag it as callback context.
        config.onNotification = [](const VnNotification* notification,
                                   void*) {
            if (notification->type == VN_NOTIFY_VOICE_ENDED) {
                (void)Vn_SetBusVolume(VN_BUS_MASTER, -3.0f, 0);
            }
        };
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();

        const std::vector<float> shortTone =
            harkentest::MakeSine(512, 440.0, 48000.0, 0.5f);
        VnPCMDesc pcm = MonoDesc(shortTone, 48000);
        ASSERT_EQ(VN_OK,
                  engine->registry.RegisterPCM(kCallbackSound, &pcm));
        const VnPlayDesc callbackPlay =
            harkentest::MakePlayDesc(kCallbackSound);
        VnVoiceHandle callbackVoice = 0;
        ASSERT_EQ(VN_OK,
                  harkentest::PlayRaw(&callbackPlay, &callbackVoice));
        ASSERT_NE(0u, callbackVoice);
        // This direct-visibility voice exists only to make Vn_Update deliver
        // a public notification. Its raw start is outside the journal and its
        // handle never becomes trace input.
        const float* channels[1] = {shortTone.data()};
        std::uint64_t contentRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, shortTone.size(), channels, 1, &contentRevision));
        ASSERT_TRUE(InstallCanonicalSoundAction(
            engine, kSound, kAction, contentRevision));
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &pcm));
        ASSERT_NE(0u, PostCanonicalAction(kAction));
        // The 512-frame voice ends within the first blocks; its ended
        // notification drains in one of these updates and fires the
        // callback mid-update.
        for (int i = 0; i < 8; ++i) {
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    // (a) The journal carries the callback-issued call, flagged, and the
    // End record aggregates the fact.
    const CallbackFlagFacts facts =
        ReadCallbackFlagFacts(tracePath, harken::kTraceCallSetBusVolume);
    EXPECT_TRUE(facts.flaggedCallFound)
        << "the callback-issued Vn_SetBusVolume was not journaled with the "
        << "callback-context flag";
    EXPECT_TRUE(facts.endAggregateSet);

    // (b) The replayer declares the limited-replay verdict — never
    // silently reorders — and (c) still completes the replay and writes
    // the capture, but under the DISTINCT exit code 3: automation cannot
    // read a limited run as ordinary deterministic success (0).
    const HostRun run =
        RunHost("trace-replay " + tracePath + " --capture " + outPath);
    EXPECT_EQ(3, run.exitCode) << run.output;
    EXPECT_FALSE(ReadFileBytes(outPath).empty());

    std::vector<std::uint8_t> realDefaultFormat =
        ReadFileBytes(tracePath);
    VnConfig callbackConfig{};
    std::uint32_t callbackVintage = 0;
    ASSERT_TRUE(ReadV3Config(realDefaultFormat, &callbackConfig,
                             &callbackVintage));
    callbackConfig.deviceMode = VN_DEVICE_REAL;
    callbackConfig.sampleRate = 0;
    callbackConfig.deviceChannelLayout = VN_CHANNEL_LAYOUT_DEFAULT;
    ASSERT_TRUE(WriteV3Config(&realDefaultFormat, callbackConfig));
    const HostRun combined = RunMutatedTrace(
        realDefaultFormat, "callback_real_default_format.vtrc");
    EXPECT_EQ(3, combined.exitCode) << combined.output;
    EXPECT_NE(std::string::npos,
              combined.output.find("device_format_identity=exact"))
        << combined.output;
}

TEST(TraceReplayE2E, SameSceneWithoutCallbackCallsReplaysAsOrdinarySuccess) {
    constexpr VnSoundId kSound = harkentest::ObjectId(1);
    constexpr VnActionId kAction = harkentest::ObjectId(2);
    constexpr VnSoundId kCallbackSound = harkentest::ObjectId(3);
    // The control for the limited-replay verdict: the identical scene with
    // no callback-issued API call replays as ordinary deterministic
    // success (exit 0, no limited/warning text).
    const std::string tracePath = TempPath("no_callback_session.vtrc");
    const std::string outPath = TempPath("no_callback_replay.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(outPath);

    {
        VnConfig config = MakeNullConfig(8, kBlockFrames);
        config.randomSeed = 3u;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();

        const std::vector<float> shortTone =
            harkentest::MakeSine(512, 440.0, 48000.0, 0.5f);
        VnPCMDesc pcm = MonoDesc(shortTone, 48000);
        ASSERT_EQ(VN_OK,
                  engine->registry.RegisterPCM(kCallbackSound, &pcm));
        const VnPlayDesc callbackPlay =
            harkentest::MakePlayDesc(kCallbackSound);
        VnVoiceHandle callbackVoice = 0;
        ASSERT_EQ(VN_OK,
                  harkentest::PlayRaw(&callbackPlay, &callbackVoice));
        ASSERT_NE(0u, callbackVoice);
        // Mirror the callback test's unjournaled notification source exactly;
        // this control has no callback that can issue a nested API call.
        const float* channels[1] = {shortTone.data()};
        std::uint64_t contentRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, shortTone.size(), channels, 1, &contentRevision));
        ASSERT_TRUE(InstallCanonicalSoundAction(
            engine, kSound, kAction, contentRevision));
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &pcm));
        ASSERT_NE(0u, PostCanonicalAction(kAction));
        for (int i = 0; i < 8; ++i) {
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    const CallbackFlagFacts facts =
        ReadCallbackFlagFacts(tracePath, harken::kTraceCallSetValue);
    EXPECT_FALSE(facts.flaggedCallFound);
    EXPECT_FALSE(facts.endAggregateSet);

    const HostRun run =
        RunHost("trace-replay " + tracePath + " --capture " + outPath);
    EXPECT_EQ(0, run.exitCode) << run.output;
    EXPECT_FALSE(ReadFileBytes(outPath).empty());
}

TEST(TraceReplayE2E, ContentHashMismatchRefusesReplayNamingTheFile) {
    const std::string packPath = TempPath("content.pack");
    const std::string tracePath = TempPath("pack_session.vtrc");
    const std::string outPath = TempPath("pack_replay.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(outPath);

    ASSERT_NO_FATAL_FAILURE(
        WriteFileBytes(packPath, BuildOneEntryPack(harkentest::ObjectId(21), 4096)));

    {
        VnConfig config = MakeNullConfig(8, kBlockFrames);
        config.randomSeed = 7u;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();

        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        VnPackDesc pack{};
        pack.structSize = sizeof(VnPackDesc);
        pack.utf8Path = packPath.c_str();
        VnPackHandle packHandle = 0;
        ASSERT_EQ(VN_OK, Vn_LoadPack(&pack, &packHandle));
        for (int i = 0; i < 8; ++i) {
            engine->nullBackend.Pump(1);
            if (i % 2 == 1) {
                ASSERT_EQ(VN_OK, Vn_Update());
            }
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    // Control: the untampered journal replays cleanly.
    const HostRun clean =
        RunHost("trace-replay " + tracePath + " --capture " + outPath);
    ASSERT_EQ(0, clean.exitCode) << clean.output;
    ASSERT_FALSE(ReadFileBytes(outPath).empty());
    std::filesystem::remove(outPath);

    // Tamper: the file's bytes no longer hash to the journal's record.
    std::vector<std::uint8_t> packBytes = ReadFileBytes(packPath);
    ASSERT_FALSE(packBytes.empty());
    packBytes.push_back(0x5A);
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(packPath, packBytes));

    const HostRun refused =
        RunHost("trace-replay " + tracePath + " --capture " + outPath);
    EXPECT_EQ(4, refused.exitCode) << refused.output;
    EXPECT_NE(std::string::npos, refused.output.find(packPath))
        << refused.output;
    // A refused replay leaves no plausible-looking capture behind.
    EXPECT_FALSE(std::filesystem::exists(outPath));
}

TEST(TraceReplayE2E, DownmixApisReplayByteIdenticalToTheSession) {
    constexpr VnSoundId kSoundId = harkentest::ObjectId(1);
    constexpr VnActionId kActionId = harkentest::ObjectId(2);
    constexpr VnBusId kBusId = harkentest::ObjectId(10);
    // The multichannel amendment's session-trace contract (§8.2): a session
    // that drives Vn_SetBusOutputLayout / Vn_SetBusDownmixMatrix /
    // Vn_ResetBusDownmixMatrix journals them faithfully, so the replay renders
    // byte-identically to the recorded session. The config is 5.1 mix -> stereo
    // device (the master folds) and a sub-bus folds to stereo through a custom
    // matrix (the per-bus fold), so both fold boundaries are exercised.
    const std::string tracePath = TempPath("downmix_session.vtrc");
    const std::string outPath = TempPath("downmix_replay.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(outPath);

    std::vector<std::uint8_t> original;
    {
        VnConfig config = MakeNullConfig(8, kBlockFrames);
        config.mixChannelLayout = VN_CHANNEL_LAYOUT_5_1;
        config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
        config.randomSeed = 424242u;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();

        const std::vector<float> tone =
            harkentest::MakeSine(8000, 220.0, 48000.0, 0.5f);
        VnPCMDesc pcm = MonoDesc(tone, 48000);
        const float* channels[1] = {tone.data()};
        std::uint64_t contentRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, tone.size(), channels, 1, &contentRevision));
        ASSERT_TRUE(InstallCanonicalSoundAction(
            engine, kSoundId, kActionId, contentRevision, kBusId));
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSoundId, &pcm));

        ASSERT_EQ(VN_OK, Vn_CreateBus(kBusId, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_SetBusOutputLayout(
                             kBusId, VN_CHANNEL_LAYOUT_STEREO));
        VnDownmixMatrixDesc mtx{};
        mtx.structSize = sizeof(VnDownmixMatrixDesc);
        mtx.srcLayout = VN_CHANNEL_LAYOUT_5_1;
        mtx.dstLayout = VN_CHANNEL_LAYOUT_STEREO;
        mtx.coeffs[0 * 8 + 0] = 1.5f; // Lo <- FL
        mtx.coeffs[1 * 8 + 1] = 0.5f; // Ro <- FR
        ASSERT_EQ(VN_OK, Vn_SetBusDownmixMatrix(kBusId, &mtx));
        ASSERT_NE(0u, PostCanonicalAction(kActionId));

        for (int i = 0; i < 20; ++i) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
            ASSERT_EQ(VN_OK, Vn_Update());
            if (i == 8) {
                // Revert the sub-bus to the ITU default mid-session; the
                // journaled reset must replay identically.
                ASSERT_EQ(VN_OK, Vn_ResetBusDownmixMatrix(kBusId));
            }
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);

    const HostRun run =
        RunHost("trace-replay " + tracePath + " --capture " + outPath);
    ASSERT_EQ(0, run.exitCode) << run.output;
    const std::vector<std::uint8_t> replay = ReadFileBytes(outPath);
    ASSERT_FALSE(replay.empty());
    ASSERT_GT(PeakOfRawF32(replay), harkentest::kSilenceFloor);
    EXPECT_EQ(original.size(), replay.size());
    EXPECT_TRUE(original == replay)
        << "downmix-session replay diverged from the recorded render";
}

TEST(TraceReplayE2E, BusMeterTapApiReplaysByteIdentically) {
    constexpr VnSoundId kSourceSound = harkentest::ObjectId(1);
    constexpr VnSoundId kTargetSound = harkentest::ObjectId(2);
    constexpr VnActionId kSourceAction = harkentest::ObjectId(3);
    constexpr VnActionId kTargetAction = harkentest::ObjectId(4);
    constexpr VnBusId kSourceBus = harkentest::ObjectId(10);
    constexpr VnBusId kTargetBus = harkentest::ObjectId(11);
    const std::string tracePath = TempPath("meter_tap_session.vtrc");
    const std::string outPath = TempPath("meter_tap_replay.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(outPath);

    std::vector<std::uint8_t> original;
    {
        EngineSession session(MakeNullConfig(4, kBlockFrames));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();

        const std::vector<float> dc = harkentest::MakeDC(48000 * 2, 0.5f);
        VnPCMDesc pcm = MonoDesc(dc, 48000);
        const float* channels[1] = {dc.data()};
        std::uint64_t contentRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, dc.size(), channels, 1, &contentRevision));
        const CanonicalSoundActionSpec actions[2] = {
            {kSourceSound, kSourceAction, contentRevision, kSourceBus},
            {kTargetSound, kTargetAction, contentRevision, kTargetBus},
        };
        ASSERT_TRUE(InstallCanonicalSoundActions(engine, actions, 2));
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSourceSound, &pcm));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kTargetSound, &pcm));
        ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));

        std::uint32_t probeGeneration = 0;
        ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                             *engine, harkentest::ObjectId(9001), kSourceBus,
                             VN_BUS_METER_TAP_PRE_FADER, 0, 0,
                             &probeGeneration));
        const harken::CurvePoint points[2] = {{-96.0f, -12.0f},
                                             {0.0f, 0.0f}};
        const harken::CurveHandlePair handles[1] = {
            {-96.0f, -12.0f, 0.0f, 0.0f}};
        ASSERT_EQ(VN_OK, harken::DefineMeterProbeBinding(
                             *engine, harkentest::ObjectId(9002), harkentest::ObjectId(9001), probeGeneration,
                             VN_TARGET_SCOPE_BUS, 0, points, handles, 2, 0));
        const std::uint32_t targetIndex = engine->buses.FindIndex(kTargetBus);
        ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                             *engine, harkentest::ObjectId(9002), VN_TARGET_SCOPE_BUS, kTargetBus, 0,
                             engine->buses.GenerationAt(targetIndex)));

        ASSERT_NE(0u, PostCanonicalAction(kSourceAction));
        ASSERT_NE(0u, PostCanonicalAction(kTargetAction));

        for (int i = 0; i < 20; ++i) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);
    const HostRun run =
        RunHost("trace-replay " + tracePath + " --capture " + outPath);
    ASSERT_EQ(0, run.exitCode) << run.output;
    const std::vector<std::uint8_t> replay = ReadFileBytes(outPath);
    ASSERT_FALSE(replay.empty());
    EXPECT_EQ(original.size(), replay.size());
    EXPECT_TRUE(original == replay)
        << "bus-meter-tap replay diverged from the recorded render";
}

TEST(TraceReplayE2E, AuthoredProximityAndToneRecipeReplayByteIdentically) {
    constexpr VnSoundId kSpatialSound = harkentest::ObjectId(1);
    constexpr VnSoundId kRecipeSound = harkentest::ObjectId(9101);
    constexpr VnActionId kSpatialAction = harkentest::ObjectId(9201);
    constexpr VnActionId kRecipeAction = harkentest::ObjectId(9202);
    constexpr VnAttenuationId kAttenuation = harkentest::ObjectId(9);
    const std::string tracePath = TempPath("proximity_tone_session.vtrc");
    const std::string outPath = TempPath("proximity_tone_replay.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(outPath);

    // Author the tone recipe pack before the traced session; its path and
    // content hash are recorded by the LoadPack event and re-read at replay.
    const harkentest::RecipePackArtifact tonePack =
        harkentest::BuildToneGainRecipePack("trace_tone_recipe");
    ASSERT_TRUE(tonePack.built);

    std::vector<std::uint8_t> original;
    {
        VnConfig config = MakeNullConfig(4, kBlockFrames);
        config.fxSlotsPerVoice = 1;
        config.gainEffectPoolCapacity = 4;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();

        const std::vector<float> tone =
            harkentest::MakeSine(48000 * 2, 330.0, 48000.0, 0.5f);
        VnPCMDesc pcm = MonoDesc(tone, 48000);
        const float* toneChannels[1] = {tone.data()};
        std::uint64_t toneRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, tone.size(), toneChannels, 1, &toneRevision));
        const std::vector<float> recipeSamples(2048, 0.5f);
        const float* recipeChannels[1] = {recipeSamples.data()};
        std::uint64_t recipeRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, recipeSamples.size(), recipeChannels, 1,
            &recipeRevision));
        const CanonicalSoundActionSpec actions[2] = {
            {kSpatialSound, kSpatialAction, toneRevision, VN_BUS_MASTER,
             true, kAttenuation},
            {kRecipeSound, kRecipeAction, recipeRevision},
        };
        ASSERT_TRUE(InstallCanonicalSoundActions(engine, actions, 2));
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSpatialSound, &pcm));

        // Authored near-field shelf with a non-default corner: the new-size
        // descriptor's proximityFrequency rides the trace and replays.
        VnAttenuationDesc attenuation = MakeAttenuationDesc(1.0f, 20.0f);
        attenuation.proximityRadius = 10.0f;
        attenuation.proximityMaxBoostDb = 12.0f;
        attenuation.proximityFrequency = 400.0f;
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(kAttenuation, &attenuation));

        VnPackDesc pack{};
        pack.structSize = sizeof(VnPackDesc);
        pack.utf8Path = tonePack.path.c_str();
        VnPackHandle packHandle = 0;
        ASSERT_EQ(VN_OK, Vn_LoadPack(&pack, &packHandle));

        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        ASSERT_EQ(VN_OK,
                  Vn_SetEmitterPosition(emitter, VnVec3{0.0f, 0.0f, -5.0f}));
        ASSERT_NE(0u, PostCanonicalAction(kSpatialAction, 0, emitter));
        ASSERT_NE(0u, PostCanonicalAction(kRecipeAction));

        for (int i = 0; i < 20; ++i) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }

    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);
    const HostRun run =
        RunHost("trace-replay " + tracePath + " --capture " + outPath);
    ASSERT_EQ(0, run.exitCode) << run.output;
    const std::vector<std::uint8_t> replay = ReadFileBytes(outPath);
    ASSERT_FALSE(replay.empty());
    EXPECT_EQ(original.size(), replay.size());
    EXPECT_TRUE(original == replay)
        << "authored proximity + tone recipe replay diverged from the "
           "recorded render";
}

TEST(TraceReplayE2E, BindingVerbsReplayByteIdenticalOnDoubleRun) {
    constexpr VnSoundId kSound = harkentest::ObjectId(1);
    constexpr VnActionId kAction = harkentest::ObjectId(2);
    const std::string tracePath = TempPath("binding_session.vtrc");
    const std::string out1 = TempPath("binding_replay1.f32");
    const std::string out2 = TempPath("binding_replay2.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(out1);
    std::filesystem::remove(out2);

    std::vector<std::uint8_t> original;
    {
        VnConfig config = MakeNullConfig(8, kBlockFrames);
        config.randomSeed = 20260714u;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const harken::VariableDefinitionV1 definitions[] = {
            harkentest::MakeVariableDefinition(harkentest::ObjectId(50), harken::VariableKindV1::Step,
                                             0.0f, 1.0f, 0.0f),
            harkentest::MakeVariableDefinition(harkentest::ObjectId(51), harken::VariableKindV1::Smooth,
                                             0.0f, 1.0f, 0.0f)};

        const std::vector<float> tone =
            harkentest::MakeSine(16000, 220.0, 48000.0, 0.5f);
        VnPCMDesc pcm = MonoDesc(tone, 48000);
        const float* channels[1] = {tone.data()};
        std::uint64_t contentRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, tone.size(), channels, 1, &contentRevision));
        const CanonicalSoundActionSpec action{
            kSound, kAction, contentRevision};
        ASSERT_TRUE(InstallCanonicalSoundActions(
            engine, &action, 1, definitions, 2));

        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &pcm));

        const VnBindingDesc busAttenuation = BindingDesc(
            harkentest::ObjectId(50), VN_TARGET_SCOPE_BUS, 0, 0.0f, -12.0f, 0);
        ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(100), &busAttenuation));
        const VnBindingAttachDesc busAttach =
            AttachDesc(VN_TARGET_SCOPE_BUS, VN_BUS_MASTER);
        ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(100), &busAttach));

        const VnBindingDesc voiceAttenuation = BindingDesc(
            harkentest::ObjectId(51), VN_TARGET_SCOPE_VOICE, 0, 0.0f, -6.0f, 80);
        ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(101), &voiceAttenuation));
        const VnBindingAttachDesc voiceAttach =
            AttachDesc(VN_TARGET_SCOPE_VOICE, kSound);
        ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(101), &voiceAttach));
        ASSERT_NE(0u, PostCanonicalAction(kAction));

        for (int i = 0; i < 30; ++i) {
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
            if (i % 2 == 0) {
                ASSERT_EQ(VN_OK, Vn_Update());
            }
            if (i == 4) {
                ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(50), 1.0f));
            }
            if (i == 6) {
                ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(51), 1.0f));
            }
            if (i == 10) {
                // Live re-curve under the frozen structural identity; the
                // revision installs at the next update and the changed
                // contribution rides the new transition.
                const VnBindingDesc recurve = BindingDesc(
                    harkentest::ObjectId(51), VN_TARGET_SCOPE_VOICE, 0, 0.0f, -24.0f, 40);
                ASSERT_EQ(VN_OK, Vn_ReplaceBinding(harkentest::ObjectId(101), &recurve));
            }
            if (i == 16) {
                ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(101), &voiceAttach));
            }
            if (i == 18) {
                ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(100), &busAttach));
                ASSERT_EQ(VN_OK, Vn_UnregisterBinding(harkentest::ObjectId(100)));
            }
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        for (int i = 0; i < 2; ++i) {
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);

    const HostRun run1 =
        RunHost("trace-replay " + tracePath + " --capture " + out1);
    ASSERT_EQ(0, run1.exitCode) << run1.output;
    const HostRun run2 =
        RunHost("trace-replay " + tracePath + " --capture " + out2);
    ASSERT_EQ(0, run2.exitCode) << run2.output;

    const std::vector<std::uint8_t> replay1 = ReadFileBytes(out1);
    const std::vector<std::uint8_t> replay2 = ReadFileBytes(out2);
    ASSERT_FALSE(replay1.empty());
    ASSERT_GT(PeakOfRawF32(replay1), harkentest::kSilenceFloor);
    EXPECT_EQ(replay1, replay2)
        << "two replays of the binding-verb journal diverged";
    EXPECT_EQ(original.size(), replay1.size());
    EXPECT_TRUE(original == replay1)
        << "binding-verb replay diverged from the recorded session's render";
}

TEST(TraceReplayE2E, CaptureQueriesReplayWithoutTrustingJournaledCapacity) {
    constexpr VnSoundId kSoundId = harkentest::ObjectId(1);
    constexpr VnActionId kActionId = harkentest::ObjectId(2);
    // The capture queries journal their full argument shape — capacity
    // included — and capacity is caller-controlled: a call refused live
    // with capacity UINT32_MAX must replay to the same refusal WITHOUT
    // the replayer sizing an allocation from the journaled number (a
    // blind vector of UINT32_MAX 48-byte rows would be an out-of-memory
    // kill, not a replay). The session records every journal shape: the
    // wrong-vintage refusal at absurd capacity, the NULL-requiredCount
    // refusal, the capacity-0 probe, a truncating fetch, and an
    // exact-fit fetch. Bars: both replays exit 0 (no OOM, no crash), report
    // capture_truncations=3, and render byte-identical to each other and to
    // the session.
    const std::string tracePath = TempPath("capture_session.vtrc");
    const std::string out1 = TempPath("capture_replay1.f32");
    const std::string out2 = TempPath("capture_replay2.f32");
    const std::string badResultPath =
        TempPath("capture_bad_result.vtrc");
    const std::string badRequiredPath =
        TempPath("capture_bad_required.vtrc");
    const std::string legacyNoTailPath =
        TempPath("capture_legacy_no_tail.vtrc");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(out1);
    std::filesystem::remove(out2);

    std::vector<std::uint8_t> original;
    {
        VnConfig config = MakeNullConfig(4, kBlockFrames);
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();

        const std::vector<float> tone =
            harkentest::MakeSine(16000, 220.0, 48000.0, 0.5f);
        VnPCMDesc pcm = MonoDesc(tone, 48000);
        const float* channels[1] = {tone.data()};
        std::uint64_t contentRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, tone.size(), channels, 1, &contentRevision));
        ASSERT_TRUE(InstallCanonicalSoundAction(
            engine, kSoundId, kActionId, contentRevision));
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSoundId, &pcm));
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(100), VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(101), VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(100), -6.0f, 0));
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(101), -3.0f, 0));
        ASSERT_NE(0u, PostCanonicalAction(kActionId));

        for (int i = 0; i < 20; ++i) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
            ASSERT_EQ(VN_OK, Vn_Update());
            if (i == 2) {
                // The hazards on purpose: readable rows, WRONG vintages,
                // absurd capacities — refused live before any write. All
                // capture row types share the bounded replay helper, so these
                // journal entries pin that no variant sizes an allocation from
                // the caller-controlled capacity.
                VnPropertyTargetInfo bad{};
                bad.structSize = sizeof(VnPropertyTargetInfo) - 4;
                std::uint32_t required = 0;
                ASSERT_EQ(VN_INVALID_ARG,
                          Vn_CapturePropertyTargets(&bad, 0xFFFFFFFFu,
                                                    &required));
                // The NULL-requiredCount refusal journals the
                // hasRequiredCount = 0 shape.
                VnPropertyTargetInfo row{};
                row.structSize = sizeof(VnPropertyTargetInfo);
                ASSERT_EQ(VN_INVALID_ARG,
                          Vn_CapturePropertyTargets(&row, 1, nullptr));
                VnEffectCapability badEffect{};
                badEffect.structSize = sizeof(VnEffectCapability) - 4;
                ASSERT_EQ(VN_INVALID_ARG,
                          Vn_CaptureEffectCapabilities(
                              &badEffect, 0xFFFFFFFFu, &required));
                VnEffectParameterCapability badParameter{};
                badParameter.structSize =
                    sizeof(VnEffectParameterCapability) - 4;
                ASSERT_EQ(VN_INVALID_ARG,
                          Vn_CaptureEffectParameterCapabilities(
                              &badParameter, 0xFFFFFFFFu, &required));
            }
            if (i == 4) {
                // Probe, truncating fetch (two bus rows into one slot —
                // counter and log record fire, side effects the replay
                // must reproduce), then an exact-fit fetch.
                std::uint32_t required = 0;
                ASSERT_EQ(VN_OK, Vn_CapturePropertyTargets(nullptr, 0,
                                                           &required));
                VnPropertyTargetInfo one{};
                one.structSize = sizeof(VnPropertyTargetInfo);
                ASSERT_EQ(VN_OK,
                          Vn_CapturePropertyTargets(&one, 1, &required));
                VnPropertyTargetInfo fit[4] = {};
                fit[0].structSize = sizeof(VnPropertyTargetInfo);
                ASSERT_EQ(VN_OK,
                          Vn_CapturePropertyTargets(fit, 4, &required));
                std::uint32_t bindingRequired = 0;
                ASSERT_EQ(VN_OK, Vn_CapturePropertyBindings(
                                     nullptr, 0, &bindingRequired));
                ASSERT_EQ(1u, engine->captureTruncationCount);

                // Both capability queries are ordinary successful calls with
                // a real one-row buffer. Their short capacities must each
                // reproduce one distinct truncation side effect on replay.
                VnEffectCapability effect{};
                effect.structSize = sizeof(VnEffectCapability);
                std::uint32_t effectRequired = 0;
                ASSERT_EQ(VN_OK, Vn_CaptureEffectCapabilities(
                                     &effect, 1, &effectRequired));
                ASSERT_EQ(2u, engine->captureTruncationCount);

                VnEffectParameterCapability parameter{};
                parameter.structSize =
                    sizeof(VnEffectParameterCapability);
                std::uint32_t parameterRequired = 0;
                ASSERT_EQ(VN_OK, Vn_CaptureEffectParameterCapabilities(
                                     &parameter, 1, &parameterRequired));
                ASSERT_EQ(3u, engine->captureTruncationCount);

                // The four appended Dynamic Modulation queries have no
                // pre-result-tail VTRC v2 representation. Probe calls keep
                // this test's truncation count unchanged while recording
                // one entry for each mandatory-tail call id.
                std::uint32_t dynamicRequired = 0;
                ASSERT_EQ(
                    VN_OK,
                    Vn_CaptureEffectDynamicCapabilities(
                        nullptr, 0, &dynamicRequired));
                ASSERT_EQ(
                    VN_OK,
                    Vn_CaptureEffectDynamicParameterCapabilities(
                        nullptr, 0, &dynamicRequired));
                ASSERT_EQ(VN_OK,
                          Vn_CaptureEffectDynamicRoutes(
                              nullptr, 0, &dynamicRequired));
                ASSERT_EQ(0u, dynamicRequired);
                ASSERT_EQ(VN_OK,
                          Vn_CaptureCompressorRuntimeInfo(
                              nullptr, 0, &dynamicRequired));
                ASSERT_EQ(0u, dynamicRequired);
                ASSERT_EQ(3u, engine->captureTruncationCount);
            }
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);

    const std::vector<std::uint8_t> recorded =
        ReadFileBytes(tracePath);
    ASSERT_FALSE(recorded.empty());
    std::vector<std::uint8_t> badResult = recorded;
    ASSERT_TRUE(PatchCaptureExpected(
        &badResult, harken::kTraceCallCapturePropertyTargets, 2,
        /*patchResult=*/true,
        static_cast<std::uint32_t>(VN_INVALID_ARG)));
    WriteFileBytes(badResultPath, badResult);
    const HostRun badResultRun =
        RunHost("trace-replay " + badResultPath);
    EXPECT_EQ(1, badResultRun.exitCode) << badResultRun.output;

    std::vector<std::uint8_t> badRequired = recorded;
    ASSERT_TRUE(PatchCaptureExpected(
        &badRequired, harken::kTraceCallCapturePropertyTargets, 2,
        /*patchResult=*/false,
        harken::kTraceRequiredCountUnavailable));
    WriteFileBytes(badRequiredPath, badRequired);
    const HostRun badRequiredRun =
        RunHost("trace-replay " + badRequiredPath);
    EXPECT_EQ(1, badRequiredRun.exitCode) << badRequiredRun.output;

    std::vector<std::uint8_t> legacyNoTail = recorded;
    const std::uint16_t legacyQueryIds[] = {
        harken::kTraceCallCapturePropertyTargets,
        harken::kTraceCallCapturePropertyBindings,
        harken::kTraceCallCaptureEffectCapabilities,
        harken::kTraceCallCaptureEffectParameterCapabilities};
    for (const std::uint16_t callId : legacyQueryIds) {
        ApiRecordLocation query;
        ASSERT_TRUE(FindApiRecord(legacyNoTail, callId, 0, &query));
        ASSERT_TRUE(EraseApiPayloadTail(
            &legacyNoTail, query,
            harken::kTraceCaptureResultTailBytes));
    }
    WriteFileBytes(legacyNoTailPath, legacyNoTail);
    const HostRun legacyNoTailRun =
        RunHost("trace-replay " + legacyNoTailPath);
    EXPECT_EQ(0, legacyNoTailRun.exitCode)
        << legacyNoTailRun.output;

    const std::uint16_t mandatoryTailQueryIds[] = {
        harken::kTraceCallCaptureEffectDynamicCapabilities,
        harken::kTraceCallCaptureEffectDynamicParameterCapabilities,
        harken::kTraceCallCaptureEffectDynamicRoutes,
        harken::kTraceCallCaptureCompressorRuntimeInfo};
    for (const std::uint16_t callId : mandatoryTailQueryIds) {
        std::vector<std::uint8_t> stripped = recorded;
        ApiRecordLocation query;
        ASSERT_TRUE(FindApiRecord(stripped, callId, 0, &query));
        ASSERT_TRUE(EraseApiPayloadTail(
            &stripped, query,
            harken::kTraceCaptureResultTailBytes));
        const std::string strippedPath =
            TempPath("capture_mandatory_tail_" +
                     std::to_string(callId) + ".vtrc");
        WriteFileBytes(strippedPath, stripped);
        const HostRun strippedRun =
            RunHost("trace-replay " + strippedPath);
        EXPECT_EQ(1, strippedRun.exitCode)
            << "call " << callId << ": " << strippedRun.output;
    }

    const HostRun run1 =
        RunHost("trace-replay " + tracePath + " --capture " + out1);
    ASSERT_EQ(0, run1.exitCode) << run1.output;
    EXPECT_NE(std::string::npos,
              run1.output.find("capture_truncations=3;"))
        << run1.output;
    const HostRun run2 =
        RunHost("trace-replay " + tracePath + " --capture " + out2);
    ASSERT_EQ(0, run2.exitCode) << run2.output;
    EXPECT_NE(std::string::npos,
              run2.output.find("capture_truncations=3;"))
        << run2.output;

    const std::vector<std::uint8_t> replay1 = ReadFileBytes(out1);
    const std::vector<std::uint8_t> replay2 = ReadFileBytes(out2);
    ASSERT_FALSE(replay1.empty());
    EXPECT_EQ(replay1, replay2)
        << "two replays of the capture-query journal diverged";
    EXPECT_EQ(original.size(), replay1.size());
    EXPECT_TRUE(original == replay1)
        << "capture-query replay diverged from the recorded session's render";
}

TEST(TraceReplayE2E,
     GainRecipeBindingsAndBypassReplayDeterministicallyTwice) {
    constexpr VnSoundId kSound = harkentest::ObjectId(9101);
    constexpr VnActionId kAction = harkentest::ObjectId(9102);
    constexpr VnBusId kBus = harkentest::ObjectId(9200);
    const std::string tracePath = TempPath("gain_recipe_session.vtrc");
    const std::string out1 = TempPath("gain_recipe_replay1.f32");
    const std::string out2 = TempPath("gain_recipe_replay2.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(out1);
    std::filesystem::remove(out2);
    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildGainRecipePack("trace_recipe_kind11", {harkentest::ObjectId(600)});
    ASSERT_TRUE(artifact.built);

    std::vector<std::uint8_t> original;
    {
        VnConfig config = MakeNullConfig(8, kBlockFrames, 48000);
        config.fxSlotsPerVoice = 1;
        config.fxSlotsPerBus = 1;
        config.gainEffectPoolCapacity = 8;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const harken::VariableDefinitionV1 definitions[] = {
            harkentest::MakeVariableDefinition(harkentest::ObjectId(700), harken::VariableKindV1::Smooth,
                                             0.0f, 1.0f, 0.0f),
            harkentest::MakeVariableDefinition(harkentest::ObjectId(701), harken::VariableKindV1::Smooth,
                                             0.0f, 1.0f, 0.0f)};
        const std::vector<float> recipeSamples(2048, 0.5f);
        const float* recipeChannels[1] = {recipeSamples.data()};
        std::uint64_t recipeRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, recipeSamples.size(), recipeChannels, 1,
            &recipeRevision));
        const CanonicalSoundActionSpec action{
            kSound, kAction, recipeRevision, kBus};
        ASSERT_TRUE(InstallCanonicalSoundActions(
            engine, &action, 1, definitions, 2));
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));

        ASSERT_EQ(VN_OK, Vn_CreateBus(kBus, VN_BUS_MASTER));
        const std::uint32_t gainLocator =
            harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0, 0);
        const VnBindingDesc voiceBinding = BindingDesc(
            harkentest::ObjectId(700), VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, gainLocator,
            0.0f, 6.0f, 0);
        const VnBindingDesc busBinding = BindingDesc(
            harkentest::ObjectId(701), VN_TARGET_SCOPE_BUS_EFFECT_PARAM, gainLocator,
            0.0f, -3.0f, 0);
        ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(600), &voiceBinding));
        ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(601), &busBinding));
        ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(700), 1.0f));
        ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(701), 1.0f));
        for (std::uint32_t step = 0;
             step < 32 && engine->variables.PendingCount() != 0; ++step) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(0u, engine->variables.PendingCount());

        VnPackDesc pack{};
        pack.structSize = sizeof(VnPackDesc);
        pack.utf8Path = artifact.path.c_str();
        VnPackHandle packHandle = 0;
        ASSERT_EQ(VN_OK, Vn_LoadPack(&pack, &packHandle));
        ASSERT_NE(0u, packHandle);

        VnGainDesc busGain{};
        busGain.structSize = sizeof(VnGainDesc);
        busGain.gainDb = 0.0f;
        ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                             kBus, 0, VN_EFFECT_GAIN,
                             reinterpret_cast<const VnEffectDesc*>(&busGain)));
        // Attach after the install: bus-effect-param attachment enforces a
        // matching installed effect on the target slot.
        const VnBindingAttachDesc busAttach =
            AttachDesc(VN_TARGET_SCOPE_BUS_EFFECT_PARAM, kBus);
        ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(601), &busAttach));
        ASSERT_NE(0u, PostCanonicalAction(kAction));

        for (int block = 0; block < 12; ++block) {
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
            ASSERT_EQ(VN_OK, Vn_Update());
            if (block == 6) {
                ASSERT_EQ(VN_OK, Vn_SetBusEffectBypass(kBus, 0, 1));
            }
            if (block == 8) {
                ASSERT_EQ(VN_OK, Vn_SetBusEffectBypass(kBus, 0, 0));
            }
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);

    const HostRun run1 =
        RunHost("trace-replay " + tracePath + " --capture " + out1);
    ASSERT_EQ(0, run1.exitCode) << run1.output;
    const HostRun run2 =
        RunHost("trace-replay " + tracePath + " --capture " + out2);
    ASSERT_EQ(0, run2.exitCode) << run2.output;
    const std::vector<std::uint8_t> replay1 = ReadFileBytes(out1);
    const std::vector<std::uint8_t> replay2 = ReadFileBytes(out2);
    ASSERT_FALSE(replay1.empty());
    ASSERT_GT(PeakOfRawF32(replay1), harkentest::kSilenceFloor);
    EXPECT_EQ(replay1, replay2);
    EXPECT_EQ(original, replay1);
}

TEST(TraceReplayE2E,
     LimiterRecipeBusParamsBypassAndFaderMoveReplayDeterministicallyTwice) {
    constexpr VnSoundId kSound = harkentest::ObjectId(9101);
    constexpr VnActionId kAction = harkentest::ObjectId(9102);
    constexpr VnBusId kBus = harkentest::ObjectId(9300);
    const std::string tracePath = TempPath("limiter_recipe_session.vtrc");
    const std::string out1 = TempPath("limiter_recipe_replay1.f32");
    const std::string out2 = TempPath("limiter_recipe_replay2.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(out1);
    std::filesystem::remove(out2);
    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildLimiterRecipePack(
            "trace_limiter_recipe_kind11", {harkentest::ObjectId(620)});
    ASSERT_TRUE(artifact.built);

    std::vector<std::uint8_t> original;
    {
        VnConfig config = MakeNullConfig(8, kBlockFrames, 48000);
        config.fxSlotsPerVoice = 1;
        config.fxSlotsPerBus = 1;
        config.limiterEffectPoolCapacity = 8;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const harken::VariableDefinitionV1 definitions[] = {
            harkentest::MakeVariableDefinition(harkentest::ObjectId(720), harken::VariableKindV1::Smooth,
                                             0.0f, 1.0f, 0.0f),
            harkentest::MakeVariableDefinition(harkentest::ObjectId(721), harken::VariableKindV1::Smooth,
                                             0.0f, 1.0f, 0.0f)};
        const std::vector<float> recipeSamples(8192, 0.5f);
        const float* recipeChannels[1] = {recipeSamples.data()};
        std::uint64_t recipeRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, recipeSamples.size(), recipeChannels, 1,
            &recipeRevision));
        const CanonicalSoundActionSpec action{
            kSound, kAction, recipeRevision, kBus};
        ASSERT_TRUE(InstallCanonicalSoundActions(
            engine, &action, 1, definitions, 2));
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));

        ASSERT_EQ(VN_OK, Vn_CreateBus(kBus, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBus, -6.020599913f, 0));
        ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kBus, 1));
        const std::uint32_t ceilingLocator =
            harken::PackEffectParamLocator(
                VN_EFFECT_LIMITER, 0, harken::kLimiterParamCeilingDb);
        const std::uint32_t releaseLocator =
            harken::PackEffectParamLocator(
                VN_EFFECT_LIMITER, 0, harken::kLimiterParamReleaseMs);
        const VnBindingDesc voiceBinding = BindingDesc(
            harkentest::ObjectId(720), VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, ceilingLocator,
            -18.0f, -6.0f, 0);
        const VnBindingDesc busBinding = BindingDesc(
            harkentest::ObjectId(721), VN_TARGET_SCOPE_BUS_EFFECT_PARAM, releaseLocator,
            20.0f, 250.0f, 0);
        ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(620), &voiceBinding));
        ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(621), &busBinding));
        ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(720), 0.0f));
        ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(721), 0.0f));
        for (std::uint32_t step = 0;
             step < 32 && engine->variables.PendingCount() != 0; ++step) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(0u, engine->variables.PendingCount());

        VnPackDesc pack{};
        pack.structSize = sizeof(VnPackDesc);
        pack.utf8Path = artifact.path.c_str();
        VnPackHandle packHandle = 0;
        ASSERT_EQ(VN_OK, Vn_LoadPack(&pack, &packHandle));
        ASSERT_NE(0u, packHandle);

        VnLimiterDesc busLimiter{};
        busLimiter.structSize = sizeof(VnLimiterDesc);
        busLimiter.ceilingDb = -12.0f;
        busLimiter.releaseMs = 100.0f;
        ASSERT_EQ(
            VN_OK,
            Vn_SetBusEffect(
                kBus, 0, VN_EFFECT_LIMITER,
                reinterpret_cast<const VnEffectDesc*>(&busLimiter)));
        const VnBindingAttachDesc busAttach =
            AttachDesc(VN_TARGET_SCOPE_BUS_EFFECT_PARAM, kBus);
        ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(621), &busAttach));
        ASSERT_NE(0u, PostCanonicalAction(kAction));

        for (int block = 0; block < 14; ++block) {
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
            ASSERT_EQ(VN_OK, Vn_Update());
            if (block == 3) {
                ASSERT_EQ(VN_OK,
                          Vn_SetBusEffectParam(
                              kBus, 0, harken::kLimiterParamCeilingDb,
                              -9.0f));
                ASSERT_EQ(VN_OK,
                          Vn_SetBusEffectParam(
                              kBus, 0, harken::kLimiterParamReleaseMs,
                              220.0f));
            }
            if (block == 6) {
                ASSERT_EQ(VN_OK, Vn_SetBusEffectBypass(kBus, 0, 1));
            }
            if (block == 7) {
                ASSERT_EQ(VN_OK, Vn_SetBusEffectBypass(kBus, 0, 0));
            }
            if (block == 8) {
                ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kBus, 0));
            }
            if (block == 9) {
                ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(720), 1.0f));
                ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(721), 1.0f));
            }
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);

    const HostRun run1 =
        RunHost("trace-replay " + tracePath + " --capture " + out1);
    ASSERT_EQ(0, run1.exitCode) << run1.output;
    const HostRun run2 =
        RunHost("trace-replay " + tracePath + " --capture " + out2);
    ASSERT_EQ(0, run2.exitCode) << run2.output;
    const std::vector<std::uint8_t> replay1 = ReadFileBytes(out1);
    const std::vector<std::uint8_t> replay2 = ReadFileBytes(out2);
    ASSERT_FALSE(replay1.empty());
    ASSERT_GT(PeakOfRawF32(replay1), harkentest::kSilenceFloor);
    EXPECT_EQ(replay1, replay2);
    EXPECT_EQ(original, replay1);
}

TEST(TraceReplayE2E,
     CompressorDynamicRoutesResetsAndCapturesReplayDeterministicallyTwice) {
    constexpr VnSoundId kSourceSound = harkentest::ObjectId(9901);
    constexpr VnSoundId kTargetSound = harkentest::ObjectId(9902);
    constexpr VnActionId kSourceAction = harkentest::ObjectId(9903);
    constexpr VnActionId kTargetAction = harkentest::ObjectId(9904);
    constexpr VnBusId kSourceBus = harkentest::ObjectId(9905);
    constexpr VnBusId kTargetBus = harkentest::ObjectId(9906);
    const std::string tracePath = TempPath("compressor_dynamic.vtrc");
    const std::string out1 = TempPath("compressor_dynamic_replay1.f32");
    const std::string out2 = TempPath("compressor_dynamic_replay2.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(out1);
    std::filesystem::remove(out2);

    std::vector<std::uint8_t> original;
    {
        VnConfig config = MakeNullConfig(4, kBlockFrames, 48000);
        config.fxSlotsPerBus = 1;
        config.compressorEffectPoolCapacity = 2;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();

        const std::vector<float> tone =
            harkentest::MakeSine(kBlockFrames * 12, 220.0, 48000.0, 0.7f);
        const VnPCMDesc pcm = MonoDesc(tone, 48000);
        const float* channels[1] = {tone.data()};
        std::uint64_t contentRevision = 0;
        ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
            48000, 1, tone.size(), channels, 1, &contentRevision));
        const CanonicalSoundActionSpec actions[2] = {
            {kSourceSound, kSourceAction, contentRevision, kSourceBus},
            {kTargetSound, kTargetAction, contentRevision, kTargetBus},
        };
        ASSERT_TRUE(InstallCanonicalSoundActions(engine, actions, 2));
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSourceSound, &pcm));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kTargetSound, &pcm));
        ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));

        VnCompressorDesc compressor{};
        compressor.structSize = sizeof compressor;
        compressor.thresholdDb = -30.0f;
        compressor.ratio = 8.0f;
        compressor.attackMs = 2.0f;
        compressor.releaseMs = 80.0f;
        compressor.kneeDb = 3.0f;
        compressor.makeupDb = 1.0f;
        ASSERT_EQ(
            VN_OK,
            Vn_SetBusEffect(
                kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                reinterpret_cast<const VnEffectDesc*>(&compressor)));

        ASSERT_NE(0u, PostCanonicalAction(kSourceAction));
        ASSERT_NE(0u, PostCanonicalAction(kTargetAction));

        VnEffectDynamicRouteDesc route{};
        route.structSize = sizeof route;
        route.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
        route.sourceKind = VN_DYNAMIC_SOURCE_BUS;
        route.tap = VN_DYNAMIC_TAP_BUS_POST_EFFECTS;
        route.sourceId = kSourceBus;
        route.levelDb = -3.0f;
        route.expectedEffectType = VN_EFFECT_COMPRESSOR;
        ASSERT_EQ(VN_OK, Vn_SetBusEffectDynamicRoute(
                             kTargetBus, 0, 0, &route));

        std::uint32_t required = 0;
        VnEffectDynamicRouteInfo routeInfo{};
        routeInfo.structSize = sizeof routeInfo;
        ASSERT_EQ(VN_OK, Vn_CaptureEffectDynamicRoutes(
                             &routeInfo, 1, &required));
        ASSERT_EQ(1u, required);
        EXPECT_EQ(VN_DYNAMIC_SOURCE_BUS, routeInfo.sourceKind);
        EXPECT_EQ(kSourceBus, routeInfo.sourceId);

        for (int block = 0; block < 8; ++block) {
            engine->nullBackend.Pump(1);
            CaptureLastBlock(engine, &original);
            ASSERT_EQ(VN_OK, Vn_Update());
            if (block == 1) {
                VnCompressorRuntimeInfo runtime[1] = {};
                runtime[0].structSize =
                    sizeof(VnCompressorRuntimeInfo);
                ASSERT_EQ(VN_OK, Vn_CaptureCompressorRuntimeInfo(
                                     runtime, 1, &required));
                ASSERT_EQ(1u, required);
            }
            if (block == 2) {
                ASSERT_EQ(VN_OK,
                          Vn_ResetBusEffect(
                              kTargetBus, 0, VN_EFFECT_COMPRESSOR));
            }
            if (block == 4) {
                ASSERT_EQ(VN_OK, Vn_ClearBusEffectDynamicRoute(
                                     kTargetBus, 0, 0,
                                     VN_EFFECT_COMPRESSOR));
            }
        }
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);

    const HostRun run1 =
        RunHost("trace-replay " + tracePath + " --capture " + out1);
    ASSERT_EQ(0, run1.exitCode) << run1.output;
    const HostRun run2 =
        RunHost("trace-replay " + tracePath + " --capture " + out2);
    ASSERT_EQ(0, run2.exitCode) << run2.output;
    const std::vector<std::uint8_t> replay1 = ReadFileBytes(out1);
    const std::vector<std::uint8_t> replay2 = ReadFileBytes(out2);
    ASSERT_FALSE(replay1.empty());
    EXPECT_EQ(replay1, replay2);
    EXPECT_EQ(original, replay1);
}

TEST(TraceReplayE2E, StateOnlyAndDefaultActionContextsReplayExactly) {
    const auto tracePath = TempPath("state_only_context.vtrc");
    const auto outPath = TempPath("state_only_context.f32");
    std::filesystem::remove(tracePath);
    std::filesystem::remove(outPath);
    std::vector<std::uint8_t> original;
    {
        EngineSession session(MakeNullConfig(8, kBlockFrames));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto* engine = session.Engine();
        std::array<std::vector<float>, 2> samples{harkentest::MakeSine(1024, 330.0, 48000.0, 0.1f),
            harkentest::MakeSine(1024, 880.0, 48000.0, 0.2f)};
        harken::PlayablePropertyProgramViewV1 properties[3]{};
        harken::ContainerChildProgramViewV2 children[2]{};
        harken::PlayableOccurrencePropertyViewV1 occurrences[2]{};
        for (unsigned i = 0; i < 3; ++i) {
            auto& property = properties[i];
            property.stableId = harkentest::ObjectId(92001 + i);
            property.contentPropertyKind = i < 2 ? harken::playableproperties::ContentPropertyKindV1::Sound :
                harken::playableproperties::ContentPropertyKindV1::Container;
            property.authoredStableOrder = i;
            property.contentRevision = 1; property.routeStableId = VN_BUS_MASTER; property.compatibilityFingerprint = i + 1;
            property.payload = ResolvedDefinitionPayload();
            if (i < 2) {
                const float* channels[] = {samples[i].data()};
                ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(48000, 1, samples[i].size(), channels, 1, &property.contentRevision));
            }
            property.programSemanticFingerprint = harken::ComputePlayablePropertyFingerprintV1(property);
        }
        for (unsigned i = 0; i < 2; ++i) {
            auto& child = children[i];
            child.stableChildOrdinal = 7 + i; child.authoredStableOrder = i; child.frequencyWeight = 1;
            child.playable = {harken::PlayableKindV1::Sound, 0, properties[i].stableId,
                properties[i].programSemanticFingerprint, properties[i].contentRevision, VN_BUS_MASTER, i + 1};
            auto& occurrence = occurrences[i];
            occurrence.parentKind = harken::playableproperties::ContentPropertyKindV1::Container;
            occurrence.parentStableId = properties[2].stableId;
            occurrence.parentProgramSemanticFingerprint = properties[2].programSemanticFingerprint;
            occurrence.stableOccurrenceOrdinal = child.stableChildOrdinal; occurrence.childPlayable = child.playable;
            occurrence.payload = EmptyOccurrencePayload();
            occurrence.occurrenceSemanticFingerprint = harken::ComputePlayableOccurrenceFingerprintV1(occurrence);
        }
        harken::ContainerProgramViewV3 container{};
        container.containerStableId = properties[2].stableId;
        container.programSemanticFingerprint = properties[2].programSemanticFingerprint;
        container.mode = harken::ContainerModeV1::Random;
        container.memoryScope = harken::MemoryScopeV1::Emitter;
        container.avoidLastN = 1; container.childCount = 2; container.children = children;
        harken::ActionStepProgramViewV1 step{};
        step.target.kind = harken::ActionTargetKindV1::Container;
        step.target.stableId = container.containerStableId;
        step.target.expectedProgramSemanticFingerprint = container.programSemanticFingerprint;
        step.delay.domain = step.fade.domain = step.cooldown.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
        step.delay.denominator = step.fade.denominator = step.cooldown.denominator = 1;
        harken::ActionProgramViewV1 program{};
        const auto action = harkentest::ObjectId(92004);
        program.actionStableId = action; program.steps = &step; program.stepCount = 1;
        program.expandedStepOccurrenceCount = program.expandedFiringCount = 1;
        program.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(program);
        harken::ContainerWorldInstallViewV1 world{};
        world.identity.capabilityMask = harken::kWorldCapabilityPlayableProperties | harken::kWorldCapabilityRandomDraw |
            harken::kWorldCapabilityActionDefinitions;
        world.playableProperties = properties; world.playablePropertyCount = 3;
        world.playableOccurrences = occurrences; world.playableOccurrenceCount = 2;
        world.containers = &container; world.containerCount = 1; world.actions = &program; world.actionCount = 1;
        world.capacity.maxActionDefinitions = 1; world.capacity.maxActionStepsPerDefinition = 1;
        world.capacity.maxActionStepsTotal = 1; world.capacity.maxActionExpandedWork = 1;
        world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV5(world);
        ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(*engine, world).status);
        ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        for (unsigned i = 0; i < 2; ++i) {
            const auto pcm = MonoDesc(samples[i], 48000);
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(properties[i].stableId, &pcm));
        }
        VnEmitterCreateDescV1 create{};
        create.structSize = sizeof(create);
        create.flags = VN_EMITTER_CREATE_STATE_ONLY;
        VnEmitterHandle state = 0, spatial = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitterEx(&create, &state));
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&spatial));
        EXPECT_EQ(0u, static_cast<std::uint32_t>(state));
        EXPECT_EQ(1u, static_cast<std::uint32_t>(spatial));
        EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, Vn_SetEmitterPosition(state, VnVec3{1, 0, 0}));
        for (unsigned turn = 0; turn < 4; ++turn) for (VnEmitterHandle emitter : {VnEmitterHandle{0}, state, spatial}) {
            ASSERT_NE(0u, PostCanonicalAction(action, 0, emitter));
            for (unsigned block = 0; block < 6; ++block) {
                ASSERT_EQ(VN_OK, Vn_Update());
                engine->nullBackend.Pump(1);
                CaptureLastBlock(engine, &original);
                ASSERT_EQ(VN_OK, Vn_Update());
            }
        }
        ASSERT_EQ(VN_OK, Vn_DestroyEmitter(state));
        ASSERT_EQ(VN_OK, Vn_DestroyEmitter(spatial));
        ASSERT_EQ(VN_OK, Vn_TraceStop());
    }
    ASSERT_GT(PeakOfRawF32(original), harkentest::kSilenceFloor);
    const auto replay = RunHost("trace-replay " + tracePath + " --capture " + outPath);
    ASSERT_EQ(0, replay.exitCode) << replay.output;
    EXPECT_EQ(original, ReadFileBytes(outPath));
}
