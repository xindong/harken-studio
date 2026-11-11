#include "Common/EffectPoolTestAccess.h"
#include "Common/DirectSoundActionWorld.h"
#include "AllocationProbe.h"
#include "harken/core/CpuClock.h"
#include "harken_preview/PreviewProject.h"
#include "Common/ObjectIdTestSupport.h"
#include "Common/StreamBuilder.h"
#include "Engine.h"
#include "PlayableRuntime.h"
#include "Common/TestUtil.h"
#include "HarkenApiInternal.h"
#include "CommandQueueSubmissionTestAccess.h"
#include "VoicePoolReleaseTestAccess.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/core/PlayablePropertyCut.h"
#include "harken/param/MeterProbe.h"
#include "harken/param/ParamTargets.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::MakeDC;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 64;
constexpr std::uint32_t kRate = 48000;
constexpr VnObjectId kVariable = harkentest::ObjectId(7);
constexpr VnObjectId kBinding = harkentest::ObjectId(9001);
constexpr VnObjectId kSound = harkentest::ObjectId(21);
constexpr VnObjectId kAction = harkentest::ObjectId(41);

struct InstalledVoiceStart {
    harken::TimelineEventPayload payload{};
    harken::TimelineProgramView program{};
    harken::EventTemplate event{};
    harken::ProgramView schedule{};

    void Publish(harken::Engine& engine, VnVoiceHandle render, std::uint64_t frames,
                 bool deferPublication = false) {
        harken::EngineTimelineVoiceInstallIdentity installed{};
        ASSERT_TRUE(engine.CaptureTimelineVoiceInstallIdentity(render, &installed));
        payload.kind = harken::TimelineEventKind::Start;
        payload.target = installed.target;
        program = {installed.rootInstance, &payload, 1};
        event.logicCommitSequence = 1;
        event.subeventPhase = 20;
        event.armToken = installed.startArm;
        schedule.ownerInstance = installed.rootInstance;
        schedule.ownerInstanceScheduleEpoch = engine.playableInstances.Classify(installed.rootInstance).scheduleEpoch;
        schedule.transactionId = 1;
        schedule.entryAnchorFrame = installed.onsetFrame;
        schedule.events = &event;
        schedule.eventCount = 1;
        schedule.runtimeProgram = {installed.rootInstance, &program};
        schedule.armToken = installed.startArm;
        harken::PostInput post{};
        post.transportEpoch = engine.timelinePool.Epoch();
        post.logicCommitSequence = 1;
        ASSERT_EQ(harken::PostStatus::Reserved, engine.timelinePlanner.PostPlay(schedule, post).status);
        if (deferPublication) return;
        engine.AcceptSourceCommandPublications();
        engine.timelinePlanner.PublishThrough(installed.onsetFrame + frames);
        engine.nullBackend.Pump(static_cast<std::uint32_t>(installed.onsetFrame / kBlock));
    }
};

VnConfig TimelineInstallConfig(std::uint32_t maxVoices = 0,
                               std::uint32_t blockFrames = 0,
                               std::uint32_t sampleRate = 0) {
    auto config = harkentest::MakeNullConfig(maxVoices, blockFrames, sampleRate);
    // Voice installation coverage does not require the default nested Action scratch.
    config.maxTriggerDepth = 2;
    return config;
}

harken::PlayablePropertyPayloadViewV1 ResolvedPayload(float spread = 0.0f) {
    harken::PlayablePropertyPayloadViewV1 payload{};
    payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    payload.values[3].bits = 1;
    payload.values[6].objectId = VN_BUS_MASTER;
    payload.values[9].bits = std::bit_cast<std::uint32_t>(spread);
    payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    return payload;
}

harken::PlayableRefV1 SoundRef(std::uint64_t contentRevision) {
    harken::PlayableRefV1 ref{};
    ref.kind = harken::PlayableKindV1::Sound;
    ref.stableId = harkentest::ObjectId(kSound);
    ref.expectedProgramSemanticFingerprint = 110;
    ref.contentRevision = contentRevision;
    ref.routeStableId = harkentest::ObjectId(kSound.low * 100);
    ref.compatibilityFingerprint = kSound.low * 1000;
    return ref;
}

harken::PlayablePropertyProgramViewV1 SoundProperty(
    const harken::PlayableRefV1& ref, float spread = 0.0f) {
    harken::PlayablePropertyProgramViewV1 row{};
    row.payload = ResolvedPayload(spread);
    row.contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Sound;
    row.stableId = ref.stableId;
    row.programSemanticFingerprint = ref.expectedProgramSemanticFingerprint;
    row.contentRevision = ref.contentRevision;
    row.routeStableId = ref.routeStableId;
    row.compatibilityFingerprint = ref.compatibilityFingerprint;
    return row;
}

VnBindingDesc MakeVoiceVolumeBinding(float y0, float y1) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = harkentest::ObjectId(kVariable);
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    desc.targetId = harken::kTargetVoiceVolume;
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, y0};
    desc.curve.points[1] = {1.0f, y1};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    return desc;
}

void SetAbsoluteTime(harken::AuthoredTimeRationalV1* time) {
    time->domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    time->numerator = 0;
    time->denominator = 1;
}

void RegisterLeaf() {
    static const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &desc));
}

void RegisterVoiceVolume(float y0) {
    const VnBindingDesc desc = MakeVoiceVolumeBinding(y0, 0.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kBinding, &desc));
}

void AttachVoiceVolume(float y0) {
    const VnBindingDesc desc = MakeVoiceVolumeBinding(y0, 0.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kBinding, &desc));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_VOICE;
    attach.objectId = harkentest::ObjectId(kSound);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kBinding, &attach));
}

void AttachVoiceVolumeRange(float y0, float y1) {
    const VnBindingDesc desc = MakeVoiceVolumeBinding(y0, y1);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kBinding, &desc));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_VOICE;
    attach.objectId = harkentest::ObjectId(kSound);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kBinding, &attach));
}

VnVoiceHandle WaitForRenderHandle(harken::Engine* engine,
                                  harken::PlayableInstanceHandle instance) {
    const auto row = std::find_if(
        engine->playableServices.begin(), engine->playableServices.end(),
        [&](const harken::EnginePlayableServiceRow& candidate) {
            return candidate.rootInstance == instance;
        });
    EXPECT_NE(engine->playableServices.end(), row);
    if (row == engine->playableServices.end()) {
        return 0;
    }
    for (std::uint32_t block = 0;
         block < 32 && row->renderHandle == 0 &&
         !engine->runtimeRecreateRequired;
         ++block) {
        EXPECT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    }
    return row->renderHandle;
}

bool BindingContribution(harken::Engine* engine, VnVoiceHandle render,
                         float* out) {
    bool found = false;
    engine->params.ForEachInstance(
        [&](const harken::ParamSystem::InstanceCaptureRow& row) {
            if (found || row.definitionId != kBinding || row.handle != render ||
                row.neutralized) {
                return;
            }
            if (out != nullptr) {
                *out = row.lastContribution;
            }
            found = true;
        });
    return found;
}

std::uint32_t BindingInstanceCount(harken::Engine* engine,
                                   VnVoiceHandle render = 0) {
    std::uint32_t count = 0;
    engine->params.ForEachInstance(
        [&](const harken::ParamSystem::InstanceCaptureRow& row) {
            if (row.definitionId != kBinding || row.neutralized) {
                return;
            }
            if (render != 0 && row.handle != render) {
                return;
            }
            ++count;
        });
    return count;
}

struct DirectWorldSoundPlayAction : harkentest::DirectSoundActionWorld {
    explicit DirectWorldSoundPlayAction(std::uint64_t contentRevision)
        : DirectSoundActionWorld(contentRevision, kSound, kAction,
            harkentest::MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth,
                0.0f, 1.0f, 0.0f), ResolvedPayload()) {}
};

harken::ResolvedPlayablePropertyCutViewV1 ResolveSoundCut(
    harken::ResolvedPlayablePropertyCutStorageV1* storage,
    std::uint64_t contentRevision, float spread = 0.0f) {
    const auto sound = SoundRef(contentRevision);
    auto property = SoundProperty(sound, spread);
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = &property;
    world.propertyCount = 1;
    EXPECT_TRUE(storage->Reset({}));
    EXPECT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage->Resolve(world, sound, nullptr, 0));
    return storage->View();
}

} // namespace

TEST(TimelineVoiceInstall, AuthoredSpreadPercentMapsOnceToRuntimeWidth) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());

    struct SpreadCase {
        float authoredPercent;
        float runtimeWidth;
    };
    constexpr std::array<SpreadCase, 6> cases{{
        {0.0f, 0.0f},
        {0.5f, 0.005f},
        {1.0f, 0.01f},
        {25.0f, 0.25f},
        {50.0f, 0.5f},
        {100.0f, 1.0f},
    }};
    for (const SpreadCase& testCase : cases) {
        SCOPED_TRACE(testCase.authoredPercent);
        harken::ResolvedPlayablePropertyCutStorageV1 storage;
        const auto cut = ResolveSoundCut(
            &storage, pcm.contentRevision, testCase.authoredPercent);
        EXPECT_FLOAT_EQ(testCase.authoredPercent, cut.spatial.spread);

        harken::TimelineVoiceInstallDesc voice{};
        ASSERT_EQ(harken::PlayableStartStatus::Accepted,
                  harken::test::BuildTimelineVoiceInstallFromCut(
                      *engine, cut, &voice));
        EXPECT_FLOAT_EQ(testCase.runtimeWidth, voice.spread);
    }
}

TEST(TimelineVoiceInstall, InvalidAuthoredSpreadIsRejectedBeforeRuntimeWidth) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());

    for (const float invalid : {
             -1.0f, 101.0f, std::numeric_limits<float>::infinity(),
             std::numeric_limits<float>::quiet_NaN()}) {
        SCOPED_TRACE(invalid);
        harken::ResolvedPlayablePropertyCutStorageV1 storage;
        auto cut = ResolveSoundCut(&storage, pcm.contentRevision);
        cut.spatial.spread = invalid;
        harken::TimelineVoiceInstallDesc voice{};
        EXPECT_EQ(harken::PlayableStartStatus::InvalidInput,
                  harken::test::BuildTimelineVoiceInstallFromCut(
                      *engine, cut, &voice));
    }
}

TEST(TimelineVoiceInstall, StartupLiveUpdateAndReplayUseSameSpreadMapping) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());

    const auto sound = SoundRef(pcm.contentRevision);
    auto property = SoundProperty(sound, 50.0f);
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = &property;
    world.propertyCount = 1;
    harken::PlayableStartRequest request{};
    request.root = sound;

    const auto startAndExpectWidth = [&](float expectedWidth) {
        const auto started = harken::StartPlayable(*engine, world, request);
        EXPECT_EQ(harken::PlayableStartStatus::Accepted, started.status);
        EXPECT_NE(0u, started.instance);
        const VnVoiceHandle render =
            WaitForRenderHandle(engine, started.instance);
        EXPECT_NE(0u, render);
        EXPECT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        const harken::VoiceRenderContext* context =
            engine->voices.Resolve(render);
        EXPECT_NE(nullptr, context);
        if (context != nullptr) {
            EXPECT_FLOAT_EQ(expectedWidth, context->width);
            EXPECT_FLOAT_EQ(0.0f, context->pan);
            EXPECT_DOUBLE_EQ(1.0, context->resampler.TargetRatio());
        }
        return std::pair{started.instance, render};
    };

    const auto first = startAndExpectWidth(0.5f);
    ASSERT_NE(0u, first.second);
    using Property = harken::playableproperties::PlayablePropertyIdV1;
    ASSERT_EQ(VN_OK, harken::SetPreviewVoiceProperty(
                         first.second, Property::SpatialSpread, 25.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    const harken::VoiceRenderContext* updated =
        engine->voices.Resolve(first.second);
    ASSERT_NE(nullptr, updated);
    EXPECT_FLOAT_EQ(0.25f, updated->width);
    EXPECT_FLOAT_EQ(0.0f, updated->pan);
    EXPECT_DOUBLE_EQ(1.0, updated->resampler.TargetRatio());

    ASSERT_EQ(harken::PlayableControlStatus::Ok,
              harken::StopPlayable(*engine, first.first, 0));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    const auto replay = startAndExpectWidth(0.5f);
    EXPECT_NE(0u, replay.second);
}

TEST(TimelineVoiceInstall, TwoDimensionalRenderDoesNotConsumeWidth) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());

    const auto sound = SoundRef(pcm.contentRevision);
    auto property = SoundProperty(sound, 100.0f);
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = &property;
    world.propertyCount = 1;
    harken::PlayableStartRequest request{};
    request.root = sound;
    const auto started = harken::StartPlayable(*engine, world, request);
    ASSERT_EQ(harken::PlayableStartStatus::Accepted, started.status);
    const VnVoiceHandle render = WaitForRenderHandle(engine, started.instance);
    ASSERT_NE(0u, render);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    const harken::VoiceRenderContext* fullWidth = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, fullWidth);
    ASSERT_FALSE(fullWidth->spatialized);
    ASSERT_FLOAT_EQ(1.0f, fullWidth->width);
    const std::size_t sampleCount =
        engine->nullBackend.BlockFrames() * engine->nullBackend.Channels();
    const std::vector<float> before(
        engine->nullBackend.LastBlock(),
        engine->nullBackend.LastBlock() + sampleCount);

    using Property = harken::playableproperties::PlayablePropertyIdV1;
    EXPECT_EQ(VN_INVALID_ARG, harken::SetPreviewVoiceProperty(
                                  render, Property::SpatialSpread, -1.0f));
    EXPECT_EQ(VN_INVALID_ARG, harken::SetPreviewVoiceProperty(
                                  render, Property::SpatialSpread, 101.0f));
    EXPECT_EQ(VN_INVALID_ARG, harken::SetPreviewVoiceProperty(
                                  render, Property::SpatialSpread,
                                  std::numeric_limits<float>::quiet_NaN()));
    ASSERT_EQ(VN_OK, harken::SetPreviewVoiceProperty(
                         render, Property::SpatialSpread, 0.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    const harken::VoiceRenderContext* zeroWidth = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, zeroWidth);
    ASSERT_FALSE(zeroWidth->spatialized);
    EXPECT_FLOAT_EQ(0.0f, zeroWidth->width);
    const std::vector<float> after(
        engine->nullBackend.LastBlock(),
        engine->nullBackend.LastBlock() + sampleCount);
    EXPECT_EQ(before, after);
}

TEST(TimelineVoiceInstall, NoLeaseCollectsAttachedVoiceBinding) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {kVariable}, 0.0f, 1.0f)
                  .status);
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolume(-6.0f));
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());

    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    const auto cut = ResolveSoundCut(&storage, pcm.contentRevision);
    harken::TimelineVoiceInstallDesc voice{};
    ASSERT_EQ(harken::PlayableStartStatus::Accepted,
              harken::test::BuildTimelineVoiceInstallFromCut(*engine, cut, &voice));
    ASSERT_EQ(1u, voice.bindingCount);
    EXPECT_EQ(kBinding, voice.bindingStorage[0].bindingId);
    EXPECT_EQ(kSound, voice.bindingStorage[0].objectId);
    EXPECT_FLOAT_EQ(-6.0f, voice.bindingStorage[0].curve.points[0].y);
}

TEST(TimelineVoiceInstall, NoLeaseCopySurvivesCatalogChange) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {kVariable}, 0.0f, 1.0f)
                  .status);
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolume(-6.0f));
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());

    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    const auto cut = ResolveSoundCut(&storage, pcm.contentRevision);
    harken::TimelineVoiceInstallDesc voice{};
    ASSERT_EQ(harken::PlayableStartStatus::Accepted,
              harken::test::BuildTimelineVoiceInstallFromCut(*engine, cut, &voice));
    ASSERT_EQ(1u, voice.bindingCount);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = cut.root.expectedProgramSemanticFingerprint;
    target.subjectId = kSound;
    harken::CapturedTimelineVoiceInstall captured;
    ASSERT_EQ(harken::PlayableStartStatus::Accepted,
        harken::test::CaptureTimelineVoiceInstallFromCut(*engine, cut, target, &captured));
    const auto frozen = captured.Input();
    ASSERT_EQ(1u, frozen.bindings.size());
    ASSERT_EQ(cut.pathCount, frozen.pathRows.size());
    EXPECT_EQ(target, frozen.facts->target);
    const auto originalPath = frozen.pathRows[0];
    EXPECT_EQ(cut.path[0].rowKind, originalPath.rowKind);
    EXPECT_EQ(cut.path[0].contentPropertyKind, originalPath.contentPropertyKind);
    EXPECT_EQ(cut.path[0].programSemanticFingerprint, originalPath.propertyFingerprint);
    EXPECT_EQ(cut.path[0].source, originalPath.source);
    EXPECT_EQ(voice.playbackPath.rows[0].source, originalPath.source);
    harken::CapturedTimelineVoiceInstall moved(std::move(captured));
    EXPECT_FALSE(captured.Valid());
    EXPECT_EQ(frozen.facts, moved.Input().facts);
    EXPECT_EQ(frozen.bindings.data(), moved.Input().bindings.data());

    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_VOICE;
    attach.objectId = harkentest::ObjectId(kSound);
    ASSERT_EQ(VN_OK, Vn_DetachBinding(kBinding, &attach));
    const VnBindingDesc replaced = MakeVoiceVolumeBinding(-12.0f, 0.0f);
    ASSERT_EQ(VN_OK, Vn_ReplaceBinding(kBinding, &replaced));

    EXPECT_EQ(kBinding, voice.bindingStorage[0].bindingId);
    EXPECT_FLOAT_EQ(-6.0f, voice.bindingStorage[0].curve.points[0].y);
    EXPECT_TRUE(storage.Reset({}));
    EXPECT_EQ(kBinding, moved.Input().bindings[0].bindingId);
    EXPECT_EQ(kSound, moved.Input().pathRows[0].stableId);
    EXPECT_EQ(originalPath.propertyFingerprint, moved.Input().pathRows[0].propertyFingerprint);
    EXPECT_EQ(originalPath.source, moved.Input().pathRows[0].source);
    EXPECT_EQ(originalPath.rowKind, moved.Input().pathRows[0].rowKind);
    EXPECT_FLOAT_EQ(-6.0f, moved.Input().bindings[0].curve.points[0].y);
    EXPECT_EQ(target, moved.Input().facts->target);
}

TEST(TimelineVoiceInstall, CapturedCutRefusalPreservesExistingDefinitionAndColumns) {
    auto config = TimelineInstallConfig(8, kBlock, kRate);
    config.voiceSendSlots = 1;
    config.fxSlotsPerVoice = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    auto cut = ResolveSoundCut(&storage, engine.registry.Lookup(kSound).contentRevision);
    harken::SendV1 send{};
    send.targetBusId = VN_BUS_MASTER;
    send.sendLevel = -9.0f;
    harken::EffectParamV1 parameter{};
    parameter.valueBits = std::bit_cast<std::uint32_t>(-6.0f);
    harken::EffectNodeV1 effect{};
    effect.effectType = VN_EFFECT_GAIN;
    effect.paramCount = 1;
    effect.params = &parameter;
    cut.sendCount = 1;
    cut.sends = &send;
    cut.effectNodeCount = 1;
    cut.effectNodes = &effect;
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = cut.root.expectedProgramSemanticFingerprint;
    target.subjectId = kSound;
    harken::CapturedTimelineVoiceInstall captured;
    ASSERT_EQ(harken::PlayableStartStatus::Accepted,
        harken::test::CaptureTimelineVoiceInstallFromCut(engine, cut, target, &captured));
    const auto before = captured.Input();
    ASSERT_EQ(1u, before.sends.size());
    ASSERT_EQ(1u, before.effects.size());
    EXPECT_FLOAT_EQ(-9.0f, before.sends[0].levelDb);
    VnGainDesc gain{};
    ASSERT_EQ(sizeof(gain), before.effects[0].descriptorSize);
    std::memcpy(&gain, before.effects[0].descriptor, sizeof(gain));
    EXPECT_FLOAT_EQ(-6.0f, gain.gainDb);
    cut.volumeDb = -18.0f;
    harken::detail::ArmLogicAllocFaultForTests(1, harken::detail::LogicAllocSite::CapturedVoiceInstall);
    const auto refused = harken::test::CaptureTimelineVoiceInstallFromCut(engine, cut, {}, &captured);
    harken::detail::DisarmLogicAllocFaultForTests();
    EXPECT_EQ(harken::PlayableStartStatus::Capacity, refused);
    EXPECT_EQ(before.facts, captured.Input().facts);
    EXPECT_FLOAT_EQ(0.0f, captured.Input().facts->volumeDb);
    EXPECT_EQ(target, captured.Input().facts->target);
    send.sendLevel = -30.0f;
    parameter.valueBits = std::bit_cast<std::uint32_t>(-24.0f);
    cut.pathCount = harken::kPlaybackPathRowCapacity + 1u;
    EXPECT_EQ(harken::PlayableStartStatus::Capacity,
        harken::test::CaptureTimelineVoiceInstallFromCut(engine, cut, {}, &captured));
    EXPECT_EQ(before.facts, captured.Input().facts);
    EXPECT_FLOAT_EQ(-9.0f, captured.Input().sends[0].levelDb);
    std::memcpy(&gain, captured.Input().effects[0].descriptor, sizeof(gain));
    EXPECT_FLOAT_EQ(-6.0f, gain.gainDb);

    harken::EnginePlayableServiceRow service;
    ASSERT_TRUE(service.PrepareFeatureArrays());
    auto* queueViews = &service.QueueRuntimeViews();
    const auto references = captured.ReferenceCount();
    queueViews->canonicalInitialQueueTargetVoice = captured.Share();
    EXPECT_EQ(references + 1, captured.ReferenceCount());
    EXPECT_EQ(before.facts, queueViews->canonicalInitialQueueTargetVoice.Input().facts);
    service.Reset();
    EXPECT_EQ(queueViews, &service.QueueRuntimeViews());
    EXPECT_FALSE(queueViews->canonicalInitialQueueTargetVoice.HasStorage());
    EXPECT_EQ(references, captured.ReferenceCount());
    EXPECT_EQ(before.facts, captured.Input().facts);
    EXPECT_EQ(before.sends.data(), captured.Input().sends.data());
    EXPECT_EQ(before.effects.data(), captured.Input().effects.data());
    EXPECT_FLOAT_EQ(-9.0f, captured.Input().sends[0].levelDb);
    std::memcpy(&gain, captured.Input().effects[0].descriptor, sizeof(gain));
    EXPECT_FLOAT_EQ(-6.0f, gain.gainDb);
}

TEST(TimelineVoiceInstall, NoLeaseAbsenceIsZeroBindings) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {kVariable}, 0.0f, 1.0f)
                  .status);
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());

    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    const auto cut = ResolveSoundCut(&storage, pcm.contentRevision);
    harken::TimelineVoiceInstallDesc voice{};
    ASSERT_EQ(harken::PlayableStartStatus::Accepted,
              harken::test::BuildTimelineVoiceInstallFromCut(*engine, cut, &voice));
    EXPECT_EQ(0u, voice.bindingCount);
}

TEST(TimelineVoiceInstall, InstallCreatesVoiceInstance) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {kVariable}, 0.0f, 1.0f)
                  .status);
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolume(-6.0f));
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());

    const auto sound = SoundRef(pcm.contentRevision);
    auto property = SoundProperty(sound);
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = &property;
    world.propertyCount = 1;
    harken::PlayableStartRequest request{};
    request.root = sound;
    const auto started = harken::StartPlayable(*engine, world, request);
    ASSERT_EQ(harken::PlayableStartStatus::Accepted, started.status);
    ASSERT_NE(0u, started.instance);

    const auto row = std::find_if(
        engine->playableServices.begin(), engine->playableServices.end(),
        [&](const harken::EnginePlayableServiceRow& candidate) {
            return candidate.rootInstance == started.instance;
        });
    ASSERT_NE(engine->playableServices.end(), row);
    for (std::uint32_t block = 0;
         block < 32 && row->renderHandle == 0 &&
         !engine->runtimeRecreateRequired;
         ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    }
    ASSERT_NE(0u, row->renderHandle);

    bool found = false;
    engine->params.ForEachInstanceOf(kBinding, [&](VnVoiceHandle handle) {
        found = found || handle == row->renderHandle;
    });
    EXPECT_TRUE(found);
}

TEST(TimelineVoiceInstall, MalformedLegacyCountsRefuseBeforeNativeAcquisition) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    harken::TimelineVoiceInstallDesc install{};
    install.soundId = kSound;
    install.outputBusId = VN_BUS_MASTER;
    const auto ledgerFree = engine.ledger.AvailableEntryCount();
    const auto voiceFree = engine.voices.AvailableSlotCount();
    const auto armFree = engine.timelineArms.FreeCount();
    const auto published = engine.commands.PublishedHead();
    const std::pair<std::uint32_t*, std::uint32_t> counts[] = {
        {&install.bindingCount, harken::kMaxVoiceBindingInstanceSlots},
        {&install.playbackPath.rowCount, harken::kPlaybackPathRowCapacity},
        {&install.sendCount, harken::kTimelineMaxVoiceSends},
        {&install.effectCount, harken::kTimelineMaxVoiceEffects},
    };
    for (const auto& [count, limit] : counts) {
        for (const auto malformed : {limit + 1u, UINT32_MAX}) {
            *count = malformed;
            EXPECT_EQ(VN_INVALID_CONFIG, harken::test::PreflightTimelineVoiceStart(engine, install));
            const auto result = harken::test::InstallTimelineVoiceStart(engine, install);
            EXPECT_EQ(VN_INVALID_CONFIG, result.status);
            EXPECT_EQ(0u, result.renderHandle);
            EXPECT_EQ(ledgerFree, engine.ledger.AvailableEntryCount());
            EXPECT_EQ(voiceFree, engine.voices.AvailableSlotCount());
            EXPECT_EQ(armFree, engine.timelineArms.FreeCount());
            EXPECT_EQ(published, engine.commands.PublishedHead());
        }
        *count = 0;
    }
}

TEST(TimelineVoiceInstall, BorrowedInputValidatesEveryActiveRangeBeforeNativeAcquisition) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    harken::TimelineVoiceInstallFacts facts{};
    facts.soundId = kSound;
    facts.outputBusId = VN_BUS_MASTER;
    const auto ledgerFree = engine.ledger.AvailableEntryCount();
    const auto voiceFree = engine.voices.AvailableSlotCount();
    const auto armFree = engine.timelineArms.FreeCount();
    const auto published = engine.commands.PublishedHead();
    const auto expectRefused = [&](const harken::TimelineVoiceInstallInput& input, VnResult status) {
        EXPECT_EQ(status, harken::test::PreflightTimelineVoiceStart(engine, input));
        const auto result = harken::test::InstallTimelineVoiceStart(engine, input);
        EXPECT_EQ(status, result.status);
        EXPECT_EQ(0u, result.renderHandle);
        EXPECT_EQ(ledgerFree, engine.ledger.AvailableEntryCount());
        EXPECT_EQ(voiceFree, engine.voices.AvailableSlotCount());
        EXPECT_EQ(armFree, engine.timelineArms.FreeCount());
        EXPECT_EQ(published, engine.commands.PublishedHead());
    };
    expectRefused({}, VN_INVALID_ARG);
    const std::array<harken::TimelineVoiceBindingInstall,
        harken::kMaxVoiceBindingInstanceSlots + 1> bindings{};
    const std::array<harken::PlaybackPathRow, harken::kPlaybackPathRowCapacity + 1> paths{};
    const std::array<harken::TimelineVoiceSendInstall, harken::kTimelineMaxVoiceSends + 1> sends{};
    const std::array<harken::TimelineVoiceEffectInstall, harken::kTimelineMaxVoiceEffects + 1> effects{};
    expectRefused({&facts, bindings}, VN_INVALID_CONFIG);
    expectRefused({&facts, {}, paths}, VN_INVALID_CONFIG);
    expectRefused({&facts, {}, {}, sends}, VN_INVALID_CONFIG);
    expectRefused({&facts, {}, {}, {}, effects}, VN_INVALID_CONFIG);
    EXPECT_EQ(VN_OK, harken::test::PreflightTimelineVoiceStart(engine,
        harken::TimelineVoiceInstallInput{&facts}));
}

TEST(TimelineVoiceInstall, BorrowedInputCopiesOnlyNativeFactsAndKeepsBindingSnapshotAfterCallerRelease) {
    for (const auto pathCount : {1u, harken::kPlaybackPathRowCapacity}) {
        SCOPED_TRACE(pathCount);
        auto config = TimelineInstallConfig(8, kBlock, kRate);
        config.voiceSendSlots = 1;
        config.fxSlotsPerVoice = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
            harkentest::InstallSmoothDefinitions(&engine, {kVariable}, 0.0f, 1.0f).status);
        ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
        const auto parent = engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0);
        ASSERT_NE(0u, parent);
        const auto clipId = harkentest::ObjectId(1235);
        const auto contentId = harkentest::ObjectId(1236);
        VnVoiceHandle render = 0;
        {
            harken::TimelineVoiceInstallFacts facts{};
            facts.soundId = kSound;
            facts.outputBusId = VN_BUS_MASTER;
            facts.capturedMedia = engine.registry.Lookup(kSound);
            facts.volumeDb = -3.0f;
            facts.pitchRatio = 1.25f;
            facts.pan = -0.25f;
            facts.spread = 0.4f;
            facts.sourceStartFrame = 11;
            facts.sourcePhase = {UINT64_C(3) << 62, 0};
            facts.authoredDurationFrames = 4096;
            facts.target.programSemanticFingerprint = 101;
            facts.target.clipId = clipId;
            facts.target.occurrenceOrdinal = 7;
            facts.monitorContent.stableId = contentId;
            facts.monitorContent.contentRevision = 103;
            facts.monitorOccurrence = {parent, clipId, 3, 7, true, true};
            std::vector<harken::TimelineVoiceBindingInstall> bindings(1);
            auto& binding = bindings.front();
            binding.objectId = kSound;
            binding.bindingId = kBinding;
            binding.variableId = kVariable;
            binding.targetId = harken::kTargetVoiceVolume;
            const harken::CurvePoint points[] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
            const harken::CurveHandlePair handles[] = {
                {points[0].x, points[0].y, points[1].x, points[1].y}};
            ASSERT_TRUE(binding.curve.SetPoints(points, handles, 2));
            std::vector<harken::PlaybackPathRow> paths(pathCount);
            for (std::uint32_t row = 0; row < pathCount; ++row) {
                paths[row] = {harkentest::ObjectId(1234 + row), 7 + row};
                paths[row].rowKind = harken::ResolvedPlayablePathRowKindV1::Definition;
                paths[row].propertyFingerprint = 100 + row;
                paths[row].source = {1, 2, 3, 4, harken::kCurrentWorldBodyVintageV20};
            }
            std::array<harken::TimelineVoiceSendInstall, 1> sends{{{VN_BUS_MASTER, -9.0f}}};
            std::array<harken::TimelineVoiceEffectInstall, 1> effects{};
            auto& effect = effects.front();
            effect.type = VN_EFFECT_GAIN;
            effect.descriptorSize = sizeof(VnGainDesc);
            VnGainDesc gain{};
            gain.structSize = sizeof(gain);
            gain.effectId = harkentest::ObjectId(1237);
            gain.gainDb = -6.0f;
            std::memcpy(effect.descriptor, &gain, sizeof(gain));
            const harken::TimelineVoiceInstallInput input{&facts, bindings, paths, sends, effects};
            ASSERT_EQ(VN_OK, harken::test::PreflightTimelineVoiceStart(engine, input));
            const auto result = harken::test::InstallTimelineVoiceStart(engine, input, parent, parent, parent);
            ASSERT_EQ(VN_OK, result.status);
            render = result.renderHandle;
            ASSERT_NE(0u, render);
            const auto owner = engine.ledger.OwnerForRender(render);
            ASSERT_TRUE(owner.live);
            harken::VoiceLedger::MonitorIdentity identity{};
            ASSERT_TRUE(engine.ledger.CaptureMonitorIdentity(owner.ledgerSlot, &identity));
            EXPECT_EQ(contentId, identity.content.content.stableId);
            EXPECT_EQ(103u, identity.content.content.contentRevision);
            EXPECT_EQ(clipId, identity.content.occurrence.clipId);
            EXPECT_EQ(3u, identity.content.occurrence.loopIteration);
            harken::EngineTimelineVoiceInstallIdentity installed{};
            ASSERT_TRUE(engine.CaptureTimelineVoiceInstallIdentity(render, &installed));
            const auto* arm = engine.timelineArms.RowAt(installed.startArm.slot);
            ASSERT_NE(nullptr, arm);
            EXPECT_EQ(facts.target, arm->target);
            ASSERT_NE(nullptr, engine.voices.Resolve(render));
            EXPECT_EQ(4096u, engine.voices.Resolve(render)->authoredSourceDeadlineFrame);
            EXPECT_EQ(facts.sourcePhase.high, engine.voices.Resolve(render)->resampler.phaseHigh);
            EXPECT_DOUBLE_EQ(0.75, engine.voices.Resolve(render)->resampler.readFraction);
            EXPECT_EQ(1u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
            std::fill(paths.begin(), paths.end(), harken::PlaybackPathRow{});
            sends = {};
            bindings.front() = {};
            facts = {};
            effects = {};
        }
        const auto observation = engine.playbackPaths[static_cast<std::uint32_t>(render)].Capture();
        ASSERT_EQ(pathCount, observation.path.rowCount);
        const auto signalPath = engine.voices.Resolve(render)->signalPath;
        ASSERT_EQ(pathCount, signalPath.size());
        ASSERT_NE(signalPath.data(), observation.path.rows);
        for (std::uint32_t row = 0; row < pathCount; ++row) {
            EXPECT_EQ(100u + row, signalPath[row].propertyFingerprint);
            EXPECT_EQ(3u, signalPath[row].source.ownerRevision);
            EXPECT_EQ(4u, signalPath[row].source.leaseIdentity);
            EXPECT_EQ(harkentest::ObjectId(1234 + row), signalPath[row].stableId);
            EXPECT_EQ(harkentest::ObjectId(1234 + row), observation.path.rows[row].stableId);
            EXPECT_EQ(7u + row, observation.path.rows[row].occurrenceOrdinal);
        }
        ASSERT_EQ(1u, observation.sendCount);
        EXPECT_EQ(VN_BUS_MASTER, observation.sends[0].targetBusId);
        EXPECT_FLOAT_EQ(-9.0f, observation.sends[0].levelDb);
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
        harkentest::AdvanceVariableWrite(&engine);
        float contribution = 0.0f;
        ASSERT_TRUE(BindingContribution(&engine, render, &contribution));
        EXPECT_FLOAT_EQ(-12.0f, contribution);
        EXPECT_EQ(1u, BindingInstanceCount(&engine, render));
        EXPECT_FALSE(engine.runtimeRecreateRequired);
    }
}

TEST(TimelineVoiceInstall, DefaultEffectBackingGrowsAcrossVoicesAndBusAndRetires) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    ASSERT_EQ(0u, engine.effectBank.CapacityOf(VN_EFFECT_LIMITER));
    VnLimiterDesc limiter{};
    limiter.structSize = sizeof(limiter);
    limiter.ceilingDb = -12.0f;
    limiter.releaseMs = 100.0f;
    std::array<harken::TimelineVoiceEffectInstall, 8> effects{};
    for (std::uint32_t slot = 0; slot < effects.size(); ++slot) {
        ASSERT_EQ(VN_OK, Vn_SetBusEffect(VN_BUS_MASTER, slot, VN_EFFECT_LIMITER,
            reinterpret_cast<const VnEffectDesc*>(&limiter)));
        effects[slot].type = VN_EFFECT_LIMITER;
        effects[slot].slot = slot;
        effects[slot].descriptorSize = sizeof(limiter);
        std::memcpy(effects[slot].descriptor, &limiter, sizeof(limiter));
    }
    harken::TimelineVoiceInstallFacts facts{};
    facts.soundId = kSound;
    facts.outputBusId = VN_BUS_MASTER;
    facts.capturedMedia = engine.registry.Lookup(kSound);
    const harken::TimelineVoiceInstallInput input{&facts, {}, {}, {}, effects};
    std::array<InstalledVoiceStart, 3> starts{};
    for (auto& start : starts) {
        const auto parent = engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0);
        ASSERT_NE(0u, parent);
        ASSERT_EQ(VN_OK, harken::test::PreflightTimelineVoiceStart(engine, input));
        const auto result = harken::test::InstallTimelineVoiceStart(engine, input, parent, parent, parent, true,
            engine.timelinePool.PublishCutoffFrame());
        ASSERT_EQ(VN_OK, result.status);
        ASSERT_NE(0u, result.renderHandle);
        ASSERT_NO_FATAL_FAILURE(start.Publish(engine, result.renderHandle, kRate, true));
    }
    ASSERT_EQ(32u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    EXPECT_GE(engine.effectBank.CapacityOf(VN_EFFECT_LIMITER), 32u);
    engine.AcceptSourceCommandPublications();
    const auto onset = starts.front().schedule.entryAnchorFrame;
    engine.timelinePlanner.PublishThrough(onset + kRate);
    engine.nullBackend.Pump(static_cast<std::uint32_t>(onset / kBlock) + 1);
    const auto count = engine.nullBackend.BlockFrames() * engine.nullBackend.Channels();
    const auto* output = engine.nullBackend.LastBlock();
    EXPECT_TRUE(std::any_of(output, output + count,
        [](float sample) { return std::abs(sample) > 0.01f; }));
    const auto ceiling = std::pow(10.0f, limiter.ceilingDb / 20.0f);
    for (std::uint32_t i = 0; i < count; ++i) EXPECT_LE(std::abs(output[i]), ceiling + 1e-5f);
    engine.nullBackend.Pump(kRate / kBlock + 16);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(8u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    for (std::uint32_t slot = 0; slot < effects.size(); ++slot)
        ASSERT_EQ(VN_OK, Vn_ClearBusEffect(VN_BUS_MASTER, slot));
    engine.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    EXPECT_FALSE(engine.runtimeRecreateRequired);
}

TEST(TimelineVoiceInstall, OutputCropUsesBoundPitchAndPreservesLoopOutroSamples) {
    for (const auto workers : {0u, 2u}) {
      for (const bool streamed : {false, true}) {
        for (const auto offset : {192u, 448u, 704u}) {
            SCOPED_TRACE(workers);
            SCOPED_TRACE(streamed);
            SCOPED_TRACE(offset);
            std::vector<float> reference;
            for (const bool crop : {false, true}) {
                auto config = TimelineInstallConfig(8, kBlock, kRate);
                config.renderWorkerCount = workers;
                config.maxStreams = 1;
                config.streamRingFrames = 4096;
                config.prebufferMs = 4;
                EngineSession session(config);
                ASSERT_EQ(VN_OK, session.InitResult());
                auto& engine = *session.Engine();
                ASSERT_EQ(harken::WorldInstallStatus::Ok,
                    harkentest::InstallSmoothDefinitions(&engine, {kVariable}, 0.0f, 1.0f).status);
                std::vector<float> pcm(1024);
                for (std::size_t i = 0; i < pcm.size(); ++i)
                    pcm[i] = 0.2f * std::sin(static_cast<float>(i) * 0.071f);
                harken::TimelineVoiceInstallFacts facts{};
                facts.soundId = kSound;
                facts.outputBusId = VN_BUS_MASTER;
                if (streamed) {
                    harkentest::StreamSpec spec;
                    spec.soundId = kSound;
                    spec.sampleRate = 44100;
                    spec.loopable = true;
                    spec.loopStartFrame = 64;
                    spec.loopEndFrame = 256;
                    const auto path = (std::filesystem::path(HARKEN_TEMP_DIR) / "compose_crop.stream").string();
                    harkentest::WriteFileBytes(path, harkentest::BuildStreamBytes(spec, pcm));
                    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(kSound, path.c_str()));
                    harken::StreamManager::StreamFacts source;
                    ASSERT_TRUE(engine.streams.FindFile(kSound, &source));
                    facts.streamed = true;
                    facts.streamFileIndex = source.fileIndex;
                    facts.streamSampleRate = source.sampleRate;
                    facts.streamChannelCount = source.channelCount;
                    facts.streamFlags = source.flags;
                    facts.streamFrameCount = source.frameCount;
                    facts.streamContentRevision = source.contentRevision;
                    facts.streamSourceGeneration = source.sourceGeneration;
                    facts.streamLoopStartFrame = source.loopStartFrame;
                    facts.streamLoopEndFrame = source.loopEndFrame;
                } else {
                    auto desc = MonoDesc(pcm, 44100);
                    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &desc));
                    facts.capturedMedia = engine.registry.Lookup(kSound);
                    facts.capturedMedia.loop = {64, 256};
                }
                facts.loopCount = 3;
                facts.pitchRatio = 1.25f;
                facts.sourcePhase = {UINT64_C(3) << 61, 0};
                facts.sourceOutputOffsetFrames = crop ? offset : 0;
                harken::TimelineVoiceBindingInstall binding{};
                binding.objectId = kSound;
                binding.bindingId = kBinding;
                binding.variableId = kVariable;
                binding.targetId = harken::kTargetVoicePitch;
                const harken::CurvePoint points[] = {{0.0f, 7.0f}, {1.0f, 7.0f}};
                const harken::CurveHandlePair handles[] = {
                    {points[0].x, points[0].y, points[1].x, points[1].y}};
                ASSERT_TRUE(binding.curve.SetPoints(points, handles, 2));
                const auto parent = engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0);
                ASSERT_NE(0u, parent);
                const harken::TimelineVoiceInstallInput input{&facts, {&binding, 1}};
                const auto result = harken::test::InstallTimelineVoiceStart(engine, input, parent, parent, parent, true,
                    engine.timelinePool.PublishCutoffFrame());
                ASSERT_EQ(VN_OK, result.status);
                ASSERT_FALSE(result.emptySource);
                ASSERT_NE(0u, result.renderHandle);
                harken::EngineTimelineVoiceInstallIdentity installed{};
                ASSERT_TRUE(engine.CaptureTimelineVoiceInstallIdentity(result.renderHandle, &installed));
                EXPECT_GT(installed.initialSourceRatio, 1.25 * 44100.0 / 48000.0);
                EXPECT_EQ(crop ? offset : 0u, facts.sourceOutputOffsetFrames);
                if (streamed) {
                    engine.streams.SyncServe();
                    engine.streams.ServiceTransferredPreparations();
                }
                InstalledVoiceStart start;
                ASSERT_NO_FATAL_FAILURE(start.Publish(engine, result.renderHandle, offset + kBlock));
                if (!crop) engine.nullBackend.Pump(offset / kBlock);
                engine.nullBackend.Pump(1);
                harken::TimelineArmObservation observation{};
                ASSERT_TRUE(engine.timelineArms.Observe(installed.startArm, &observation));
                ASSERT_NE(0u, observation.startedBlockSequence)
                    << "state=" << static_cast<unsigned>(observation.state)
                    << " applied=" << observation.armedCompletionSequence
                    << " final=" << observation.finalSourcePublished
                    << " onset=" << installed.onsetFrame
                    << " protocol=" << engine.mixer.CommandProtocolViolationCount();
                const auto count = engine.nullBackend.BlockFrames() * engine.nullBackend.Channels();
                const auto* output = engine.nullBackend.LastBlock();
                ASSERT_TRUE(std::any_of(output, output + count,
                    [](float sample) { return std::abs(sample) > 0.01f; }));
                if (!crop) reference.assign(output, output + count);
                else {
                    ASSERT_EQ(reference.size(), count);
                    for (std::size_t i = 0; i < count; ++i)
                        EXPECT_NEAR(reference[i], output[i], 1e-6f) << i;
                }
                EXPECT_FALSE(engine.runtimeRecreateRequired);
            }
        }
      }
    }
}

TEST(TimelineVoiceInstall, OneFrameStreamRingRejectsMissingNeighborButAcceptsNaturalSingleFrameAndEmptyCrop) {
    for (const auto workers : {0u, 2u}) for (const auto sourceFrames : {1u, 8u}) {
        SCOPED_TRACE(::testing::Message() << "workers=" << workers << " frames=" << sourceFrames);
        auto config = TimelineInstallConfig(2, kBlock, kRate);
        config.renderWorkerCount = workers;
        config.maxStreams = 1;
        config.streamRingFrames = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const auto voicesBefore = engine.voices.AvailableSlotCount();
        harkentest::StreamSpec spec;
        spec.soundId = kSound;
        const auto path = (std::filesystem::path(HARKEN_TEMP_DIR) / "compose_ring_one.stream").string();
        harkentest::WriteFileBytes(path, harkentest::BuildStreamBytes(spec, std::vector<float>(sourceFrames, 0.25f)));
        ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(kSound, path.c_str()));
        harken::StreamManager::StreamFacts source;
        ASSERT_TRUE(engine.streams.FindFile(kSound, &source));
        harken::TimelineVoiceInstallFacts facts{};
        facts.soundId = kSound;
        facts.outputBusId = VN_BUS_MASTER;
        facts.streamed = true;
        facts.streamFileIndex = source.fileIndex;
        facts.streamSampleRate = source.sampleRate;
        facts.streamChannelCount = source.channelCount;
        facts.streamFlags = source.flags;
        facts.streamFrameCount = source.frameCount;
        facts.streamContentRevision = source.contentRevision;
        facts.streamSourceGeneration = source.sourceGeneration;
        facts.sourceOutputOffsetFrames = sourceFrames;
        const auto parent = engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0);
        ASSERT_NE(0u, parent);
        const harken::TimelineVoiceInstallInput input{&facts};
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            const auto empty = harken::test::InstallTimelineVoiceStart(engine, input, parent, parent, parent, true);
            ASSERT_EQ(VN_OK, empty.status);
            EXPECT_TRUE(empty.emptySource);
            EXPECT_EQ(0u, empty.renderHandle);
            EXPECT_EQ(voicesBefore, engine.voices.AvailableSlotCount());
            EXPECT_EQ(1u, engine.streams.FreeSlotCount());
        }
        facts.sourceOutputOffsetFrames = 0;
        const auto installed = harken::test::InstallTimelineVoiceStart(engine, input, parent, parent, parent, true,
            engine.timelinePool.PublishCutoffFrame());
        if (sourceFrames != 1) {
            EXPECT_EQ(VN_NO_STREAM, installed.status);
            EXPECT_EQ(0u, installed.renderHandle);
            EXPECT_EQ(voicesBefore, engine.voices.AvailableSlotCount());
            EXPECT_EQ(1u, engine.streams.FreeSlotCount());
            continue;
        }
        ASSERT_EQ(VN_OK, installed.status);
        engine.streams.SyncServe();
        engine.streams.ServiceTransferredPreparations();
        InstalledVoiceStart start;
        ASSERT_NO_FATAL_FAILURE(start.Publish(engine, installed.renderHandle, kBlock));
        engine.nullBackend.Pump(1);
        EXPECT_NEAR(0.25f, engine.nullBackend.LastBlock()[0], 0.00001f);
        for (unsigned sample = 2; sample < kBlock * 2; ++sample)
            EXPECT_FLOAT_EQ(0.0f, engine.nullBackend.LastBlock()[sample]);
        EXPECT_FALSE(engine.runtimeRecreateRequired);
    }
}

TEST(TimelineVoiceInstall, EmptyOutputCropRollsBackPrivateResourcesAndAllowsReuse) {
    EngineSession session(TimelineInstallConfig(2, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    const auto parent = engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0);
    ASSERT_NE(0u, parent);
    harken::TimelineVoiceInstallFacts facts{};
    facts.soundId = kSound;
    facts.outputBusId = VN_BUS_MASTER;
    facts.capturedMedia = engine.registry.Lookup(kSound);
    facts.sourceOutputOffsetFrames = facts.capturedMedia.frames;
    const harken::TimelineVoiceInstallInput input{&facts};
    for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
        const auto result = harken::test::InstallTimelineVoiceStart(engine, input, parent, parent, parent, true);
        ASSERT_EQ(VN_OK, result.status);
        EXPECT_TRUE(result.emptySource);
        EXPECT_EQ(0u, result.renderHandle);
        EXPECT_EQ(0u, facts.capturedMedia.useCount->load());
        EXPECT_FALSE(engine.runtimeRecreateRequired);
    }
    facts.sourceOutputOffsetFrames = 0;
    const auto result = harken::test::InstallTimelineVoiceStart(engine, input, parent, parent, parent, true,
        engine.timelinePool.PublishCutoffFrame());
    ASSERT_EQ(VN_OK, result.status);
    ASSERT_NE(0u, result.renderHandle);
    EXPECT_FALSE(result.emptySource);
    InstalledVoiceStart start;
    ASSERT_NO_FATAL_FAILURE(start.Publish(engine, result.renderHandle, kBlock));
    engine.nullBackend.Pump(1);
    EXPECT_GT(engine.nullBackend.LastBlock()[0], 0.0f);
}

TEST(TimelineVoiceInstall, BorrowedInputFailureUnwindsNativePrefixAndAllowsFreshRetry) {
    auto config = TimelineInstallConfig(8, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    const auto parent = engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0);
    ASSERT_NE(0u, parent);
    harken::TimelineVoiceInstallFacts facts{};
    facts.soundId = kSound;
    facts.outputBusId = VN_BUS_MASTER;
    std::array<harken::TimelineVoiceEffectInstall, 2> effects{};
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    for (auto& effect : effects) {
        effect.type = VN_EFFECT_GAIN;
        effect.descriptorSize = sizeof(gain);
        std::memcpy(effect.descriptor, &gain, sizeof(gain));
    }
    effects[1].slot = config.fxSlotsPerVoice;
    const auto ledgerFree = engine.ledger.AvailableEntryCount();
    const auto voiceFree = engine.voices.AvailableSlotCount();
    const auto armFree = engine.timelineArms.FreeCount();
    const auto published = engine.commands.PublishedHead();
    const harken::TimelineVoiceInstallInput input{&facts, {}, {}, {}, effects};
    const auto refused = harken::test::InstallTimelineVoiceStart(engine, input, parent, parent, parent);
    EXPECT_EQ(VN_INVALID_ARG, refused.status);
    EXPECT_EQ(0u, refused.renderHandle);
    EXPECT_EQ(ledgerFree, engine.ledger.AvailableEntryCount());
    EXPECT_EQ(voiceFree, engine.voices.AvailableSlotCount());
    EXPECT_EQ(armFree, engine.timelineArms.FreeCount());
    EXPECT_EQ(published, engine.commands.PublishedHead());
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    const std::array<harken::PlaybackPathRow, 2> paths{{
        {harkentest::ObjectId(1234), 7}, {harkentest::ObjectId(1235), 8}}};
    const harken::TimelineVoiceInstallInput retry{&facts, {}, paths, {}, std::span{effects}.first(1)};
    harken::detail::ArmLogicAllocFaultForTests(1, harken::detail::LogicAllocSite::PlaybackPathStorage);
    const auto storageRefused = harken::test::InstallTimelineVoiceStart(engine, retry, parent, parent, parent);
    EXPECT_EQ(0u, harken::detail::g_logicAllocFault.remaining);
    harken::detail::DisarmLogicAllocFaultForTests();
    EXPECT_EQ(VN_BACKPRESSURE, storageRefused.status);
    EXPECT_EQ(0u, storageRefused.renderHandle);
    EXPECT_EQ(ledgerFree, engine.ledger.AvailableEntryCount());
    EXPECT_EQ(voiceFree, engine.voices.AvailableSlotCount());
    EXPECT_EQ(armFree, engine.timelineArms.FreeCount());
    EXPECT_EQ(published, engine.commands.PublishedHead());
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(VN_OK, harken::test::PreflightTimelineVoiceStart(engine, retry));
    const auto installed = harken::test::InstallTimelineVoiceStart(engine, retry, parent, parent, parent);
    EXPECT_EQ(VN_OK, installed.status);
    EXPECT_NE(0u, installed.renderHandle);
    EXPECT_EQ(1u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_FALSE(engine.runtimeRecreateRequired);
}

TEST(TimelineVoiceInstall, FirstSampleUsesCurrentVariableValue) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {kVariable}, 0.0f, 1.0f)
                  .status);
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolumeRange(0.0f, -12.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());

    const auto sound = SoundRef(pcm.contentRevision);
    auto property = SoundProperty(sound);
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = &property;
    world.propertyCount = 1;
    harken::PlayableStartRequest request{};
    request.root = sound;
    const auto started = harken::StartPlayable(*engine, world, request);
    ASSERT_EQ(harken::PlayableStartStatus::Accepted, started.status);
    ASSERT_NE(0u, started.instance);
    const VnVoiceHandle render = WaitForRenderHandle(engine, started.instance);
    ASSERT_NE(0u, render);

    float contribution = 0.0f;
    ASSERT_TRUE(BindingContribution(engine, render, &contribution));
    EXPECT_FLOAT_EQ(-12.0f, contribution);
}

TEST(TimelineVoiceInstall, LiveSetValueUpdatesContribution) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {kVariable}, 0.0f, 1.0f)
                  .status);
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolumeRange(0.0f, -12.0f));
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());

    const auto sound = SoundRef(pcm.contentRevision);
    auto property = SoundProperty(sound);
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = &property;
    world.propertyCount = 1;
    harken::PlayableStartRequest request{};
    request.root = sound;
    const auto started = harken::StartPlayable(*engine, world, request);
    ASSERT_EQ(harken::PlayableStartStatus::Accepted, started.status);
    ASSERT_NE(0u, started.instance);
    const VnVoiceHandle render = WaitForRenderHandle(engine, started.instance);
    ASSERT_NE(0u, render);

    float before = 0.0f;
    ASSERT_TRUE(BindingContribution(engine, render, &before));
    EXPECT_FLOAT_EQ(0.0f, before);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    float after = 0.0f;
    ASSERT_TRUE(BindingContribution(engine, render, &after));
    EXPECT_FLOAT_EQ(-12.0f, after);
}

TEST(TimelineVoiceInstall, StopRetiresVoiceBindingInstance) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {kVariable}, 0.0f, 1.0f)
                  .status);
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolumeRange(0.0f, -12.0f));
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());

    const auto sound = SoundRef(pcm.contentRevision);
    auto property = SoundProperty(sound);
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = &property;
    world.propertyCount = 1;
    harken::PlayableStartRequest request{};
    request.root = sound;
    const auto started = harken::StartPlayable(*engine, world, request);
    ASSERT_EQ(harken::PlayableStartStatus::Accepted, started.status);
    ASSERT_NE(0u, started.instance);
    const VnVoiceHandle render = WaitForRenderHandle(engine, started.instance);
    ASSERT_NE(0u, render);
    ASSERT_TRUE(BindingContribution(engine, render, nullptr));

    ASSERT_EQ(harken::PlayableControlStatus::Ok,
              harken::StopPlayable(*engine, started.instance, 0));
    bool live = true;
    for (std::uint32_t block = 0; block < 128 && live; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        live = false;
        engine->params.ForEachInstanceOf(kBinding, [&](VnVoiceHandle handle) {
            live = live || handle == render;
        });
    }
    EXPECT_FALSE(live);
}

TEST(TimelineVoiceInstall, DirectWorldPostActionAttachesVoiceBinding) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());
    DirectWorldSoundPlayAction image(pcm.contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harken::InstallContainerWorldV1(*engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolume(-6.0f));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = harkentest::ObjectId(kAction);
    desc.emitter = emitter;
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    ASSERT_NE(0u, firing);

    VnActionStepResult play{};
    play.structSize = sizeof(play);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &play));
    ASSERT_NE(0u, play.producedPlayableInstance);
    const VnVoiceHandle render =
        WaitForRenderHandle(engine, play.producedPlayableInstance);
    ASSERT_NE(0u, render);
    EXPECT_NE(0u, BindingInstanceCount(engine, render));
}

TEST(TimelineVoiceInstall, SpatialActionFirstBlockCombinesInitialContributions) {
    struct StartCase { std::uint32_t precedingAudioBlocks; std::uint32_t delayMode; };
    for (const auto test : {StartCase{0, 0}, StartCase{2, 0}, StartCase{0, 1}, StartCase{0, 2}}) {
        SCOPED_TRACE(test.precedingAudioBlocks);
        SCOPED_TRACE(test.delayMode);
        EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
        const VnAttenuationId attenuation = harkentest::ObjectId(73);
        const auto profile = harkentest::MakeAttenuationDesc(1.0f, 101.0f, VN_ROLLOFF_LINEAR);
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(attenuation, &profile));
        DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
        image.property.payload.presentMask |= UINT64_C(1);
        image.property.payload.values[0].bits = std::bit_cast<std::uint32_t>(-6.0f);
        image.property.payload.values[10].bits = 1;
        image.property.payload.values[13].objectId = attenuation;
        image.property.programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(image.property);
        image.step.target.expectedProgramSemanticFingerprint =
            image.property.programSemanticFingerprint;
        if (test.delayMode == 1) image.step.delay.numerator = 4000000;
        const auto deferredFrame = engine.timelinePool.WindowEndFrame();
        if (test.delayMode == 2) image.step.delay.numerator =
            static_cast<std::int64_t>((deferredFrame * UINT64_C(1000000000) + kRate - 1) / kRate);
        if (test.delayMode != 0) {
            image.action.directFeatureMask = 1;
            image.action.transitiveFeatureMask = 1;
        }
        image.action.programSemanticFingerprint =
            harken::ComputeActionProgramFingerprintV1(image.action);
        image.world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(image.world);
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
            harken::InstallContainerWorldV1(engine, image.world).status);
        ASSERT_NO_FATAL_FAILURE(AttachVoiceVolume(-6.0f));
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(emitter,
            {test.delayMode == 2 ? -51.0f : 51.0f, 0.0f, 0.0f}));
        ASSERT_EQ(VN_OK, Vn_Update());
        engine.nullBackend.Pump(test.precedingAudioBlocks);
        VnPostActionDesc desc{};
        desc.structSize = sizeof(desc);
        desc.actionId = kAction;
        desc.emitter = emitter;
        VnActionFiringHandle firing = 0;
        ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
        ASSERT_NE(0u, firing);
        if (test.delayMode == 2) {
            ASSERT_EQ(1u, engine.actionDeferredRecords.LiveCount());
            ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, {51.0f, 0.0f, 0.0f}));
        }
        bool checkedFarInstall = false;
        if (test.delayMode == 2) {
            engine.immediateEntryAfterPublishContextForTest = &checkedFarInstall;
            engine.immediateEntryAfterPublishHookForTest = [](harken::Engine& current, void* context) {
                auto& checked = *static_cast<bool*>(context);
                if (checked) return;
                for (const auto& row : current.playableServices) {
                    harken::Spatializer::SpatialVoiceState spatial{};
                    if (row.renderHandle == 0 ||
                        !current.spatial.CaptureVoice(row.renderHandle, &spatial) || !spatial.spatialized) continue;
                    checked = true;
                    EXPECT_FLOAT_EQ(51.0f, spatial.posX);
                    EXPECT_FLOAT_EQ(1.0f, current.params.ResolvedValue(row.renderHandle, harken::kTargetVoicePan));
                    EXPECT_NEAR(-12.0f + 20.0f * std::log10(0.5f),
                        current.params.ResolvedValue(row.renderHandle, harken::kTargetVoiceVolume), 1e-5f);
                    break;
                }
            };
        }
        harkentest::Capture captured;
        const auto blocks = test.delayMode == 2 ? deferredFrame / kBlock + 16u : 16u;
        for (std::uint32_t block = 0; block < blocks; ++block) {
            harkentest::PumpAndCapture(&engine, 1, captured);
            if (test.delayMode == 2) {
                ASSERT_EQ(VN_OK, Vn_Update());
            } else {
                // ACK/onset/Variable publication is serviced without spatial evaluation.
                engine.timelineService->Service();
            }
        }
        if (test.delayMode == 2) EXPECT_TRUE(checkedFarInstall);
        engine.immediateEntryAfterPublishHookForTest = nullptr;
        engine.immediateEntryAfterPublishContextForTest = nullptr;
        const auto audible = std::find_if(captured.right.begin(), captured.right.end(),
            [](float value) { return std::abs(value) > harkentest::kSilenceFloor; });
        VnActionStepResult step{};
        step.structSize = sizeof(step);
        ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
        ASSERT_NE(captured.right.end(), audible)
            << "step=" << step.state << " result=" << step.result
            << " deferred=" << engine.actionDeferredRecords.LiveCount()
            << " instances=" << engine.playableInstances.LiveCount()
            << " recreate=" << engine.runtimeRecreateRequired
            << " leftPeak=" << harkentest::Peak(captured.left)
            << " rendered=" << engine.mixer.BlocksRendered()
            << " window=" << engine.timelinePool.WindowEndFrame()
            << " cutoff=" << engine.timelinePool.PublishCutoffFrame();
        const auto first = static_cast<std::uint32_t>(audible - captured.right.begin());
        const float expectedRight = 0.5f * std::pow(10.0f, -12.0f / 20.0f) *
            0.5f * std::sqrt(2.0f);
        for (std::uint32_t frame = first; frame < (first / kBlock + 1) * kBlock; ++frame) {
            ASSERT_NEAR(0.0f, captured.left[frame], 1e-5f) << frame;
            ASSERT_NEAR(expectedRight, captured.right[frame], 1e-5f) << frame;
        }
    }
}

TEST(TimelineVoiceInstall, DirectWorldPostActionWithoutAttachmentHasNoInstance) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    const auto pcm = engine->registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());
    DirectWorldSoundPlayAction image(pcm.contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harken::InstallContainerWorldV1(*engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(RegisterVoiceVolume(-6.0f));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = harkentest::ObjectId(kAction);
    desc.emitter = emitter;
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    ASSERT_NE(0u, firing);

    for (std::uint32_t block = 0; block < 32 && !engine->runtimeRecreateRequired;
         ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    }
    EXPECT_EQ(0u, BindingInstanceCount(engine));
}

TEST(TimelineVoiceInstall, PendingSoundReinstallKeepsCapturedCatalogAndReturnsOwner) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolume(-6.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(2);
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = kAction;
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    const auto row = std::find_if(engine.playableServices.begin(), engine.playableServices.end(),
        [&](const auto& service) {
            return service.InvocationValid() && service.Invocation().actionFiring == firing;
        });
    ASSERT_NE(engine.playableServices.end(), row);
    ASSERT_TRUE(row->usesCapturedVoice);
    const auto frozen = row->capturedVoice.Input();
    ASSERT_EQ(1u, frozen.bindings.size());
    const auto original = row->renderHandle;
    const auto release = row->releaseObserver;
    ASSERT_NE(0u, original);
    ASSERT_TRUE(row->pendingImmediateEntry);
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_VOICE;
    attach.objectId = kSound;
    ASSERT_EQ(VN_OK, Vn_DetachBinding(kBinding, &attach));
    const auto replaced = MakeVoiceVolumeBinding(-12.0f, 0.0f);
    ASSERT_EQ(VN_OK, Vn_ReplaceBinding(kBinding, &replaced));
    ASSERT_TRUE(engine.playablePropertyCutScratch.Reset({}));
    ASSERT_EQ(VN_OK, Vn_Update());
    for (unsigned block = 0; block < 8 && !row->immediateLane.Valid(); ++block) {
        engine.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    ASSERT_TRUE(row->immediateLane.Valid());
    while (engine.mixer.BlocksRendered() < row->startFrame / kBlock)
        engine.nullBackend.Pump(1);
    engine.mixer.ArenaForTests().SetCapacityForTests(0);
    engine.nullBackend.Pump(1);
    engine.mixer.ArenaForTests().RestoreCapacityForTests();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    bool reinstalled = false;
    bool heard = false;
    for (unsigned block = 0; block < 64 && !heard; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_TRUE(row->capturedVoice.Valid());
        EXPECT_EQ(frozen.facts, row->capturedVoice.Input().facts);
        EXPECT_EQ(frozen.bindings.data(), row->capturedVoice.Input().bindings.data());
        EXPECT_FLOAT_EQ(-6.0f, row->capturedVoice.Input().bindings[0].curve.points[0].y);
        reinstalled |= row->renderHandle != 0 && row->renderHandle != original;
        engine.nullBackend.Pump(1);
        heard = harkentest::Peak(engine.nullBackend.LastBlock(), 2 * kBlock) > 0.01f;
    }
    ASSERT_TRUE(reinstalled);
    ASSERT_TRUE(heard);
    EXPECT_TRUE(engine.voices.ReleaseObserved(release));
    EXPECT_EQ(nullptr, engine.voices.Resolve(original));
    float contribution = 0.0f;
    ASSERT_TRUE(BindingContribution(&engine, row->renderHandle, &contribution));
    EXPECT_FLOAT_EQ(-6.0f, contribution);
    const auto expected = 0.5f * std::pow(10.0f, -6.0f / 20.0f);
    for (std::uint32_t frame = kBlock / 2; frame < kBlock; ++frame)
        EXPECT_NEAR(expected, engine.nullBackend.LastBlock()[2 * frame], 1e-6f);
    ASSERT_EQ(VN_OK, Vn_StopActionFiring(firing, 0));
    for (unsigned block = 0; block < 128 && row->capturedVoice.Valid(); ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine.nullBackend.Pump(1);
    }
    EXPECT_FALSE(row->capturedVoice.Valid());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    EXPECT_FALSE(engine.runtimeRecreateRequired);
}

static void CheckOrderedActionFirstPcm(bool sameAction, bool fixedStart = false,
    std::int64_t delayNanoseconds = 4000000, bool delayedWrites = false,
    bool refuseAudioArena = false) {
    EngineSession session(TimelineInstallConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    if (fixedStart) {
        image.step.delay.numerator = delayNanoseconds;
        image.action.directFeatureMask = image.action.transitiveFeatureMask = 1;
    }
    std::array<harken::ActionStepProgramViewV1, 4> ordered{};
    if (sameAction) {
        ordered[0] = image.step;
        ordered[0].opcode = harken::ActionOpcodeV1::SetVariable;
        ordered[0].delay.numerator = delayedWrites ? image.step.delay.numerator : 0;
        ordered[0].target = {};
        ordered[0].target.kind = harken::ActionTargetKindV1::Variable;
        ordered[0].target.stableId = kVariable;
        ordered[0].valueBits = std::bit_cast<std::uint32_t>(0.0f);
        ordered[1] = image.step;
        ordered[2] = ordered[0];
        ordered[2].valueBits = std::bit_cast<std::uint32_t>(1.0f);
        ordered[3] = image.step;
        for (std::uint32_t i = 0; i < ordered.size(); ++i) ordered[i].authoredEventIndex = i;
        image.action.steps = ordered.data();
        image.action.stepCount = static_cast<std::uint32_t>(ordered.size());
        image.action.expandedStepOccurrenceCount = image.action.stepCount;
    }
    image.action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(image.action);
    image.world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(image.world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolumeRange(0, -12));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(2);
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = kAction;
    std::array<VnActionFiringHandle, 2> firings{};
    if (sameAction) {
        ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firings[0]));
        firings[1] = firings[0];
    } else {
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0));
        ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firings[0]));
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firings[1]));
    }
    std::array<VnVoiceHandle, 2> renders{};
    for (std::uint32_t i = 0; i < renders.size(); ++i) {
        VnActionStepResult play{};
        play.structSize = sizeof(play);
        ASSERT_EQ(VN_OK, Vn_QueryActionStep(firings[i], sameAction ? 1u + 2u * i : 0u, &play));
        ASSERT_NE(0u, play.producedPlayableInstance);
        const auto row = std::find_if(engine.playableServices.begin(), engine.playableServices.end(),
            [&](const auto& service) { return service.rootInstance == play.producedPlayableInstance; });
        ASSERT_NE(engine.playableServices.end(), row);
        if (fixedStart) EXPECT_FALSE(row->pendingImmediateEntry);
        renders[i] = row->renderHandle;
        ASSERT_NE(0u, renders[i]);
    }
    if (delayedWrites) {
        for (std::uint32_t update = 0; update < 3; ++update) {
            ASSERT_EQ(VN_OK, Vn_Update());
            for (const auto& row : engine.playableServices) {
                if (row.renderHandle != renders[0] && row.renderHandle != renders[1]) continue;
                harken::TimelineArmObservation arm{};
                ASSERT_TRUE(engine.timelineArms.Observe(row.arm, &arm));
                EXPECT_EQ(harken::ArmState::PendingApply, arm.state);
                EXPECT_LE(engine.timelinePool.PublishCutoffFrame(), (row.startFrame / kBlock) * kBlock);
            }
        }
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    if (refuseAudioArena) {
        engine.mixer.ArenaForTests().SetCapacityForTests(0);
        for (std::uint32_t block = 0; block < 4; ++block) {
            engine.nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
            for (std::uint32_t sample = 0; sample < 2 * kBlock; ++sample)
                EXPECT_EQ(0, engine.nullBackend.LastBlock()[sample]);
            for (const auto render : renders)
                EXPECT_TRUE(engine.voices.HasTimelineVariableSourceRead(static_cast<std::uint32_t>(render),
                    static_cast<std::uint32_t>(render >> 32)));
        }
        engine.mixer.ArenaForTests().RestoreCapacityForTests();
    }
    bool heard = false;
    for (std::uint32_t step = 0; step < 24 && !heard; ++step) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine.nullBackend.Pump(1);
        heard = std::abs(engine.nullBackend.LastBlock()[2 * (kBlock - 1)]) > 1e-7f;
    }
    ASSERT_TRUE(heard);
    const auto initialA = engine.voices.Resolve(renders[0]);
    const auto initialB = engine.voices.Resolve(renders[1]);
    ASSERT_NE(nullptr, initialA);
    ASSERT_NE(nullptr, initialB);
    for (const auto render : renders)
        EXPECT_FALSE(engine.voices.HasTimelineVariableSourceRead(static_cast<std::uint32_t>(render),
            static_cast<std::uint32_t>(render >> 32)));
    const auto b = std::pow(10.0f, -12.0f / 20.0f);
    EXPECT_NEAR(1, initialA->gain.current, 1e-6f);
    EXPECT_NEAR(b, initialB->gain.current, 1e-6f);
    for (std::uint32_t i = kBlock / 2; i < kBlock; ++i)
        EXPECT_NEAR(0.5f * (1 + b), engine.nullBackend.LastBlock()[2 * i], 1e-6f);
    engine.nullBackend.Pump(1);
    for (std::uint32_t i = kBlock / 2; i < kBlock; ++i)
        EXPECT_NEAR(b, engine.nullBackend.LastBlock()[2 * i], 1e-6f);
    ASSERT_EQ(VN_OK, Vn_Update());
    for (const auto render : renders) EXPECT_FALSE(engine.variables.HasNativePublicationHold(render));
}

TEST(TimelineVoiceInstall, ExternalWritesAndConsecutiveActionsKeepOwnInitialPcm) {
    CheckOrderedActionFirstPcm(false);
}

TEST(TimelineVoiceInstall, SameActionWritesAndPlaysKeepOwnInitialPcm) {
    CheckOrderedActionFirstPcm(true);
}

TEST(TimelineVoiceInstall, FixedSoundExternalWritesKeepOwnInitialPcm) {
    CheckOrderedActionFirstPcm(false, true);
}

TEST(TimelineVoiceInstall, FixedSoundSameActionWritesKeepOwnInitialPcm) {
    CheckOrderedActionFirstPcm(true, true);
}

TEST(TimelineVoiceInstall, FixedSoundNonalignedExternalWritesKeepOwnInitialPcm) {
    CheckOrderedActionFirstPcm(false, true, 4250000);
}

TEST(TimelineVoiceInstall, FixedSoundNonalignedSameActionWritesKeepOwnInitialPcm) {
    CheckOrderedActionFirstPcm(true, true, 4250000);
}

TEST(TimelineVoiceInstall, DelayedSameFrameWritesAndPlaysKeepOwnInitialPcm) {
    CheckOrderedActionFirstPcm(true, true, 4000000, true);
}

TEST(TimelineVoiceInstall, DelayedNonalignedSameFrameWritesAndPlaysKeepOwnInitialPcm) {
    CheckOrderedActionFirstPcm(true, true, 4250000, true);
}

TEST(TimelineVoiceInstall, PendingExternalWritesKeepOwnInitialPcmAcrossAudioArenaRefusal) {
    CheckOrderedActionFirstPcm(false, false, 4000000, false, true);
}

TEST(TimelineVoiceInstall, PendingSameActionWritesKeepOwnInitialPcmAcrossAudioArenaRefusal) {
    CheckOrderedActionFirstPcm(true, false, 4000000, false, true);
}

TEST(TimelineVoiceInstall, CancelFixedSameFramePlayReleasesPageAndKeepsVariableWrite) {
    EngineSession session(TimelineInstallConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    image.step.delay.numerator = 4250000;
    std::array<harken::ActionStepProgramViewV1, 2> steps{image.step, image.step};
    steps[0].opcode = harken::ActionOpcodeV1::SetVariable;
    steps[0].target = {};
    steps[0].target.kind = harken::ActionTargetKindV1::Variable;
    steps[0].target.stableId = kVariable;
    steps[0].valueBits = std::bit_cast<std::uint32_t>(1.0f);
    steps[1].authoredEventIndex = 1;
    image.action.steps = steps.data();
    image.action.stepCount = image.action.expandedStepOccurrenceCount = 2;
    image.action.directFeatureMask = image.action.transitiveFeatureMask = 1;
    image.action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(image.action);
    image.world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(image.world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolumeRange(0, -12));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(2);
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = kAction;
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    VnActionStepResult play{};
    play.structSize = sizeof(play);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 1, &play));
    const auto row = std::find_if(engine.playableServices.begin(), engine.playableServices.end(),
        [&](const auto& service) { return service.rootInstance == play.producedPlayableInstance; });
    ASSERT_NE(engine.playableServices.end(), row);
    const auto render = row->renderHandle;
    const auto sourceFrame = row->startFrame;
    harken::VoiceReleaseObserver release{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(render, &release));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_LE(engine.timelinePool.PublishCutoffFrame(), (sourceFrame / kBlock) * kBlock);
    ASSERT_EQ(harken::PlayableControlStatus::Ok, harken::StopPlayable(engine, play.producedPlayableInstance, 0));
    for (std::uint32_t block = 0; block < 24; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine.nullBackend.Pump(1);
        for (std::uint32_t sample = 0; sample < 2 * kBlock; ++sample)
            EXPECT_EQ(0.0f, engine.nullBackend.LastBlock()[sample]);
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    float value = 0.0f;
    ASSERT_TRUE(engine.variables.CurrentValue(kVariable, &value));
    EXPECT_FLOAT_EQ(1.0f, value);
    EXPECT_GT(engine.timelinePool.PublishCutoffFrame(), sourceFrame);
    EXPECT_TRUE(engine.voices.ReleaseObserved(release));
    EXPECT_FALSE(engine.voices.HasTimelineVariableSourceRead(static_cast<std::uint32_t>(render),
        static_cast<std::uint32_t>(render >> 32)));
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    EXPECT_FALSE(engine.runtimeRecreateRequired);
}

TEST(TimelineVoiceInstall, OlderProbeWithdrawalCannotPublishUninstalledFixedSoundPage) {
    EngineSession session(TimelineInstallConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    image.step.delay.numerator = 500000;
    image.action.directFeatureMask = image.action.transitiveFeatureMask = 1;
    image.action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(image.action);
    image.world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(image.world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolumeRange(0, -12));

    constexpr auto sourceBus = harkentest::ObjectId(22);
    constexpr auto targetBus = harkentest::ObjectId(23);
    constexpr auto probeId = harkentest::ObjectId(8001);
    constexpr auto probeBinding = harkentest::ObjectId(8002);
    ASSERT_EQ(VN_OK, Vn_CreateBus(sourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(targetBus, VN_BUS_MASTER));
    std::uint32_t probeGeneration = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(engine, probeId, sourceBus,
        VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, &probeGeneration));
    const harken::CurvePoint points[2] = {{-96, -6}, {0, -6}};
    const harken::CurveHandlePair handles[1] = {{-96, -6, 0, -6}};
    ASSERT_EQ(VN_OK, harken::DefineMeterProbeBinding(engine, probeBinding, probeId,
        probeGeneration, VN_TARGET_SCOPE_BUS, harken::kBusTargetBusVolume, points, handles, 2, 0));
    const auto targetIndex = engine.buses.FindIndex(targetBus);
    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(engine, probeBinding, VN_TARGET_SCOPE_BUS,
        targetBus, 0, engine.buses.GenerationAt(targetIndex)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());

    // Capture a real older frontier, leaving its command unconsumed.
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(targetBus, -3, 0));
    const auto olderFrontier = engine.commands.PublishedHead();
    ASSERT_GT(olderFrontier, engine.commands.SettledFrontier());
    ASSERT_EQ(VN_OK, harken::UninstallMeterProbe(engine, probeId, probeGeneration));
    ASSERT_EQ(VN_OK, Vn_Update());
    auto* withdrawal = engine.meterDetectors.Find(probeId);
    ASSERT_NE(nullptr, withdrawal);
    ASSERT_TRUE(withdrawal->withdrawalFrontierCaptured);
    ASSERT_EQ(olderFrontier, withdrawal->withdrawalCommandFrontier);
    ASSERT_FALSE(withdrawal->withdrawalPageCommitted);
    engine.nullBackend.Pump(1);
    ASSERT_GE(engine.commands.SettledFrontier(), olderFrontier);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0));
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = kAction;
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    VnActionStepResult play{};
    play.structSize = sizeof(play);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &play));
    const auto row = std::find_if(engine.playableServices.begin(), engine.playableServices.end(),
        [&](const auto& service) { return service.rootInstance == play.producedPlayableInstance; });
    ASSERT_NE(engine.playableServices.end(), row);
    const auto render = row->renderHandle;
    ASSERT_NE(0u, render);
    const auto startPage = (row->startFrame / kBlock) * kBlock;
    ASSERT_GT(engine.commands.PublishedHead(), engine.commands.SettledFrontier());
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_TRUE(withdrawal->withdrawalPageCommitted);
    const auto withdrawalSlice = engine.meterProbes.WithdrawalSliceForTests(engine, probeId, probeGeneration);
    ASSERT_TRUE(withdrawalSlice.Valid());
    harken::TimelineArmObservation arm{};
    ASSERT_TRUE(engine.timelineArms.Observe(row->arm, &arm));
    EXPECT_EQ(harken::ArmState::PendingApply, arm.state);
    EXPECT_LE(engine.timelinePool.PublishCutoffFrame(), startPage);

    bool produced = false;
    for (std::uint32_t block = 0; block < 24; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine.nullBackend.Pump(1);
        if (std::abs(engine.nullBackend.LastBlock()[2 * (kBlock - 1)]) > 1e-7f) {
            produced = true;
            EXPECT_NEAR(0.5f,
                engine.nullBackend.LastBlock()[2 * (kBlock - 1)], 1e-6f);
        }
    }
    EXPECT_TRUE(produced);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_GT(engine.timelinePool.PublishCutoffFrame(), startPage);
    EXPECT_EQ(nullptr, engine.meterDetectors.Find(probeId));
    EXPECT_EQ(0u, engine.meterProbeWithdrawalCount);
    EXPECT_EQ(0u, engine.meterProbes.AttachmentCount(engine));
    EXPECT_EQ(0u, engine.meterProbes.BindingCount(engine));
    EXPECT_FLOAT_EQ(-3, engine.busTargets.LastCommitted(targetIndex, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine.voices.HasTimelineVariableSourceRead(static_cast<std::uint32_t>(render),
        static_cast<std::uint32_t>(render >> 32)));
    EXPECT_FALSE(engine.runtimeRecreateRequired);
}

TEST(TimelineVoiceInstall, PendingSoundIgnoresEarlierSequenceFutureWriteUntilItIsDue) {
    EngineSession session(TimelineInstallConfig(2, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolumeRange(0, -12));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(2);
    auto observed = harken::ObserveVariableCallAt(engine.variables.CurrentWorldGeneration(),
        engine.variables.CurrentTransportEpoch(), kBlock, engine.mixer.BlocksRendered(),
        engine.commands.PublishedHead());
    observed.requestedEffectiveFrame = 20 * kBlock;
    const auto future = engine.variables.AcceptGlobal(kVariable, 1, observed,
        engine.params, engine.bindings, engine.busTargets);
    ASSERT_EQ(VN_OK, future.result);
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = kAction;
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    VnActionStepResult play{};
    play.structSize = sizeof(play);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &play));
    const auto row = std::find_if(engine.playableServices.begin(), engine.playableServices.end(),
        [&](const auto& service) { return service.rootInstance == play.producedPlayableInstance; });
    ASSERT_NE(engine.playableServices.end(), row);
    ASSERT_TRUE(row->pendingImmediateEntry);
    const auto render = row->renderHandle;
    ASSERT_NE(0u, render);
    bool produced = false;
    for (std::uint32_t block = 0; block < 16 && !produced; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine.nullBackend.Pump(1);
        produced = std::abs(engine.nullBackend.LastBlock()[2 * (kBlock - 1)]) > 1e-7f;
    }
    ASSERT_TRUE(produced);
    ASSERT_LT(engine.mixer.BlocksRendered() * kBlock, observed.requestedEffectiveFrame);
    EXPECT_NEAR(0.5f, engine.nullBackend.LastBlock()[2 * (kBlock - 1)], 1e-6f);
    while (engine.mixer.BlocksRendered() < 24) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine.nullBackend.Pump(1);
    }
    EXPECT_NEAR(0.5f * std::pow(10.0f, -12.0f / 20.0f),
        engine.nullBackend.LastBlock()[2 * (kBlock - 1)], 1e-6f);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FALSE(engine.variables.HasNativePublicationHold(render));
    EXPECT_FALSE(engine.voices.HasTimelineVariableSourceRead(static_cast<std::uint32_t>(render),
        static_cast<std::uint32_t>(render >> 32)));
    EXPECT_FALSE(engine.runtimeRecreateRequired);
}

static void CheckPendingSoundEarlyReleaseAndReuse(bool destroySourceEmitter) {
    auto config = TimelineInstallConfig(1, kBlock, kRate);
    config.maxEmitters = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolumeRange(0, -12));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(2);
    for (std::uint32_t attempt = 0; attempt < 3; ++attempt) {
        SCOPED_TRACE(attempt);
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, 1));
        VnPostActionDesc desc{};
        desc.structSize = sizeof(desc);
        desc.actionId = kAction;
        desc.emitter = emitter;
        VnActionFiringHandle firing = 0;
        ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
        VnActionStepResult play{};
        play.structSize = sizeof(play);
        ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &play));
        const auto row = std::find_if(engine.playableServices.begin(), engine.playableServices.end(),
            [&](const auto& service) { return service.rootInstance == play.producedPlayableInstance; });
        ASSERT_NE(engine.playableServices.end(), row);
        ASSERT_TRUE(row->pendingImmediateEntry);
        const auto render = row->renderHandle;
        ASSERT_NE(0u, render);
        ASSERT_TRUE(engine.voices.HasTimelineVariableSourceRead(static_cast<std::uint32_t>(render),
            static_cast<std::uint32_t>(render >> 32)));
        harken::VoiceReleaseObserver release{};
        ASSERT_TRUE(engine.voices.CaptureReleaseObserver(render, &release));
        if (destroySourceEmitter) ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
        else ASSERT_EQ(harken::PlayableControlStatus::Ok,
            harken::StopPlayable(engine, play.producedPlayableInstance, 0));
        for (std::uint32_t block = 0; block < 24; ++block) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine.nullBackend.Pump(1);
            for (std::uint32_t sample = 0; sample < 2 * kBlock; ++sample)
                EXPECT_EQ(0, engine.nullBackend.LastBlock()[sample]);
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_TRUE(engine.voices.ReleaseObserved(release));
        EXPECT_FALSE(engine.variables.HasNativePublicationHold(render));
        EXPECT_FALSE(engine.voices.HasTimelineVariableSourceRead(static_cast<std::uint32_t>(render),
            static_cast<std::uint32_t>(render >> 32)));
        EXPECT_EQ(0u, BindingInstanceCount(&engine));
        EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
        if (!destroySourceEmitter) ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));

        VnEmitterHandle freshEmitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&freshEmitter));
        EXPECT_NE(emitter, freshEmitter);
        EXPECT_EQ(static_cast<std::uint32_t>(emitter), static_cast<std::uint32_t>(freshEmitter));
        desc.emitter = freshEmitter;
        ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
        ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &play));
        bool produced = false;
        for (std::uint32_t block = 0; block < 16 && !produced; ++block) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine.nullBackend.Pump(1);
            produced = std::abs(engine.nullBackend.LastBlock()[2 * (kBlock - 1)]) > 1e-7f;
        }
        ASSERT_TRUE(produced);
        EXPECT_NEAR(0.5f, engine.nullBackend.LastBlock()[2 * (kBlock - 1)], 1e-6f);
        EXPECT_TRUE(engine.voices.ReleaseObserved(release));
        ASSERT_EQ(harken::PlayableControlStatus::Ok,
            harken::StopPlayable(engine, play.producedPlayableInstance, 0));
        ASSERT_EQ(VN_OK, Vn_DestroyEmitter(freshEmitter));
        for (std::uint32_t block = 0; block < 16; ++block) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine.nullBackend.Pump(1);
        }
        EXPECT_FALSE(engine.runtimeRecreateRequired);
    }
}

TEST(TimelineVoiceInstall, PendingSourceEmitterFailureReleasesReaderAndAllowsFreshReuse) {
    CheckPendingSoundEarlyReleaseAndReuse(true);
}

TEST(TimelineVoiceInstall, PendingStopBeforeInstallationReleasesReaderAndAllowsFreshReuse) {
    CheckPendingSoundEarlyReleaseAndReuse(false);
}

TEST(TimelineVoiceInstall, PendingReadCutCapacityRefusalRollsBackAndKeepsAcceptedWrite) {
    EngineSession session(TimelineInstallConfig(1, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolumeRange(0, -12));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
    const auto commandHead = engine.commands.PublishedHead();
    const auto freeRows = engine.variableSlices.FreeRowCount();
    const auto freeTargets = engine.variableSlices.FreeTargetCapacity();
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = kAction;
    VnActionFiringHandle firing = 0;
    harken::detail::ArmLogicAllocFaultForTests(2, harken::detail::LogicAllocSite::VariableReadCutColumns);
    EXPECT_EQ(VN_BACKPRESSURE, Vn_PostAction(&desc, &firing));
    harken::detail::DisarmLogicAllocFaultForTests();
    EXPECT_EQ(0u, firing);
    EXPECT_EQ(commandHead, engine.commands.PublishedHead());
    EXPECT_EQ(freeRows, engine.variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine.variableSlices.FreeTargetCapacity());
    EXPECT_EQ(0u, BindingInstanceCount(&engine));
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    std::vector<harken::VariableReadCutRef> held(engine.variables.ReadCutCapacity());
    for (auto& cut : held) ASSERT_TRUE(engine.variables.CaptureReadCut(0, 8 * kBlock,
        engine.logicCommits.Current() + 1u, 0, &cut));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_PostAction(&desc, &firing));
    EXPECT_EQ(0u, firing);
    EXPECT_EQ(commandHead, engine.commands.PublishedHead());
    EXPECT_EQ(freeRows, engine.variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine.variableSlices.FreeTargetCapacity());
    EXPECT_EQ(0u, BindingInstanceCount(&engine));
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    for (const auto cut : held) ASSERT_TRUE(engine.variables.ReleaseReadCut(cut));
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    bool produced = false;
    for (std::uint32_t block = 0; block < 16 && !produced; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine.nullBackend.Pump(1);
        produced = std::abs(engine.nullBackend.LastBlock()[2 * (kBlock - 1)]) > 1e-7f;
    }
    ASSERT_TRUE(produced);
    EXPECT_NEAR(0.5f * std::pow(10.0f, -12.0f / 20.0f),
        engine.nullBackend.LastBlock()[2 * (kBlock - 1)], 1e-6f);
    EXPECT_FALSE(engine.runtimeRecreateRequired);
}

TEST(TimelineVoiceInstall, PendingTransportResetReleasesReaderBeforeFreshEpochPlayback) {
    EngineSession session(TimelineInstallConfig(1, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolumeRange(0, -12));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = kAction;
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    VnActionStepResult play{};
    play.structSize = sizeof(play);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &play));
    const auto row = std::find_if(engine.playableServices.begin(), engine.playableServices.end(),
        [&](const auto& service) { return service.rootInstance == play.producedPlayableInstance; });
    ASSERT_NE(engine.playableServices.end(), row);
    ASSERT_TRUE(row->pendingImmediateEntry);
    const auto render = row->renderHandle;
    harken::VoiceReleaseObserver release{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(render, &release));
    const auto oldEpoch = engine.variables.CurrentTransportEpoch();
    const auto resetFrame = engine.mixer.BlocksRendered() * kBlock;
    auto reset = harken::BeginRuntimeTransportReset(engine, resetFrame);
    for (std::uint32_t block = 0; block < 24 && reset == harken::RuntimeTransportResetStatus::Backpressure; ++block) {
        engine.nullBackend.Pump(1);
        for (std::uint32_t sample = 0; sample < 2 * kBlock; ++sample)
            EXPECT_EQ(0, engine.nullBackend.LastBlock()[sample]);
        ASSERT_EQ(VN_OK, Vn_Update());
        reset = harken::BeginRuntimeTransportReset(engine, resetFrame);
    }
    ASSERT_EQ(harken::RuntimeTransportResetStatus::Complete, reset);
    EXPECT_GT(engine.variables.CurrentTransportEpoch(), oldEpoch);
    EXPECT_TRUE(engine.voices.ReleaseObserved(release));
    EXPECT_FALSE(engine.variables.HasNativePublicationHold(render));
    EXPECT_FALSE(engine.voices.HasTimelineVariableSourceRead(static_cast<std::uint32_t>(render),
        static_cast<std::uint32_t>(render >> 32)));
    EXPECT_EQ(0u, BindingInstanceCount(&engine));
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0));
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    bool produced = false;
    for (std::uint32_t block = 0; block < 16 && !produced; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine.nullBackend.Pump(1);
        produced = std::abs(engine.nullBackend.LastBlock()[2 * (kBlock - 1)]) > 1e-7f;
    }
    ASSERT_TRUE(produced);
    EXPECT_NEAR(0.5f, engine.nullBackend.LastBlock()[2 * (kBlock - 1)], 1e-6f);
    EXPECT_FALSE(engine.runtimeRecreateRequired);
}

static void CheckFixedSoundMissedDeadline(bool lateInstallation) {
    EngineSession session(TimelineInstallConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    image.step.delay.numerator = 4000000;
    image.action.directFeatureMask = image.action.transitiveFeatureMask = 1;
    image.action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(image.action);
    image.world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(image.world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolumeRange(0, -12));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0));
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = kAction;
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    VnActionStepResult play{};
    play.structSize = sizeof(play);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &play));
    const auto row = std::find_if(engine.playableServices.begin(), engine.playableServices.end(),
        [&](const auto& service) { return service.rootInstance == play.producedPlayableInstance; });
    ASSERT_NE(engine.playableServices.end(), row);
    ASSERT_FALSE(row->pendingImmediateEntry);
    const auto arm = row->arm;
    const auto render = row->renderHandle;
    const auto startQuantum = row->startFrame / kBlock;
    if (lateInstallation) engine.mixer.SetMaxCommandsPerQuantumForTest(0);
    while (engine.mixer.BlocksRendered() <= startQuantum) engine.nullBackend.Pump(1);
    if (lateInstallation) {
        engine.mixer.SetMaxCommandsPerQuantumForTest(engine.config.maxControlCommandsPerQuantum);
        engine.nullBackend.Pump(1);
    }
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(arm, &observed));
    ASSERT_EQ(harken::ArmState::Published, observed.state);
    ASSERT_TRUE(observed.finalSourcePublished);
    ASSERT_NE(0u, observed.armedCompletionSequence);
    ASSERT_EQ(0u, observed.startedBlockSequence);
    if (lateInstallation) ASSERT_GT(observed.armedCompletionSequence, startQuantum);
    else ASSERT_LE(observed.armedCompletionSequence, startQuantum);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::EnginePlayableStartPhase::Cancelling, row->phase);
    EXPECT_EQ(harken::PlayableControlReason::Deadline, row->controlReason);
    for (std::uint32_t step = 0; step < 16; ++step) {
        engine.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        float peak = 0.0f;
        for (std::uint32_t sample = 0; sample < 2 * kBlock; ++sample)
            peak = std::max(peak, std::abs(engine.nullBackend.LastBlock()[sample]));
        EXPECT_EQ(0.0f, peak);
    }
    EXPECT_FALSE(engine.variables.HasNativePublicationHold(render));
    EXPECT_FALSE(engine.runtimeRecreateRequired);
}

TEST(TimelineVoiceInstall, FixedSoundLateInstallationFailsDeadlineWithoutPcm) {
    CheckFixedSoundMissedDeadline(true);
}

TEST(TimelineVoiceInstall, FixedSoundStalePublicationFailsDeadlineWithoutPcm) {
    CheckFixedSoundMissedDeadline(false);
}

TEST(TimelineVoiceInstall, SnapshotOwnedSurvivesCallerStorageOverwrite) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {kVariable}, 0.0f, 1.0f)
                  .status);
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    VnPlayDesc play = harkentest::MakePlayDesc(kSound);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    ASSERT_NE(0u, render);

    harken::Curve curve;
    const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
    const harken::CurveHandlePair handles[] = {
        {pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
    std::uint32_t transitionMs = 50;
    std::uint32_t revision = 1;
    harken::BindingDefinitionView view{};
    view.definitionId = harkentest::ObjectId(kBinding);
    view.variableId = harkentest::ObjectId(kVariable);
    view.targetId = harken::kTargetVoiceVolume;
    view.curve = &curve;
    view.transitionMs = &transitionMs;
    view.revision = &revision;
    view.snapshotOwned = true;
    ASSERT_EQ(harken::ParamSystem::InstanceBindResult::Ok,
              engine->params.BindVoiceInstance(view, render, kSound, false));

    curve = harken::Curve{};
    transitionMs = 99;
    revision = 99;

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    float contribution = 0.0f;
    ASSERT_TRUE(BindingContribution(engine, render, &contribution));
    EXPECT_FLOAT_EQ(-12.0f, contribution);
}

TEST(TimelineVoiceInstall, PreviewRefreshIsExplicitAndRespectsPitchGuard) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {kVariable}, 0.0f, 1.0f).status);
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    auto desc = MakeVoiceVolumeBinding(0, 1200);
    desc.targetId = harken::kTargetVoicePitch;
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kBinding, &desc));
    const auto play = harkentest::MakePlayDesc(kSound);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const auto render = harkentest::RenderHandle(engine, handle);
    auto view = engine->bindings.ViewOf(*engine->bindings.Find(kBinding));
    view.snapshotOwned = true;
    ASSERT_EQ(harken::ParamSystem::InstanceBindResult::Ok,
              engine->params.BindVoiceInstance(view, render, kSound, false));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    const auto expectContribution = [&](float expected) {
        EXPECT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        float value = 0;
        EXPECT_TRUE(BindingContribution(engine, render, &value));
        EXPECT_FLOAT_EQ(expected, value);
    };
    expectContribution(1200);
    desc.curve.points[1].y = 2400;
    desc.curve.handles[0].h2y = 2400;
    ASSERT_EQ(VN_OK, Vn_ReplaceBinding(kBinding, &desc));
    expectContribution(1200);
    EXPECT_FALSE(engine->params.RefreshPreviewBindings(render, engine->bindings, false));
    expectContribution(1200);
    EXPECT_TRUE(engine->params.RefreshPreviewBindings(render, engine->bindings, true));
    expectContribution(2400);
    desc.curve.points[1].y = 3600;
    desc.curve.handles[0].h2y = 3600;
    ASSERT_EQ(VN_OK, Vn_ReplaceBinding(kBinding, &desc));
    expectContribution(2400);
    EXPECT_TRUE(engine->params.RefreshPreviewBindings(render, engine->bindings, true));
    expectContribution(3600);
    EXPECT_EQ(1u, BindingInstanceCount(engine, render));
}

namespace {
using SourceRegistry = harken::OriginalSourceSubmissionRegistry;
std::vector<harken::TimelineArmToken> PublishedSourceTokens(harken::Engine& engine) {
    std::vector<harken::TimelineArmToken> tokens;
    for (std::uint32_t i = 0; i < engine.timelineArms.Capacity(); ++i) {
        const auto* arm = engine.timelineArms.RowAt(i);
        if (arm == nullptr) continue;
        const harken::TimelineArmToken token{i, arm->generation, arm->renderSlot, arm->renderGeneration};
        const auto* source = engine.originalSourceSubmissions.Find(token);
        if (source != nullptr && source->State() == SourceRegistry::Phase::Published)
            tokens.push_back(token);
    }
    return tokens;
}
VnResult PostSourceAction() {
    VnEmitterHandle emitter = 0;
    if (const auto result = Vn_CreateEmitter(&emitter); result != VN_OK) return result;
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = kAction;
    desc.emitter = emitter;
    VnActionFiringHandle firing = 0;
    return Vn_PostAction(&desc, &firing);
}
void InstallTwoSourceAction(harken::Engine& engine, DirectWorldSoundPlayAction& image,
    std::array<harken::ActionStepProgramViewV1, 2>& steps) {
    steps.fill(image.step);
    steps[1].authoredEventIndex = 1;
    image.action.steps = steps.data();
    image.action.stepCount = 2;
    image.action.expandedStepOccurrenceCount = 2;
    image.action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(image.action);
    image.world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(image.world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
}
float FreshSourcePeak(harken::Engine& engine) {
    float peak = 0;
    for (std::uint32_t i = 0; i < 8; ++i) {
        EXPECT_EQ(VN_OK, Vn_Update());
        engine.nullBackend.Pump(1);
        const auto* block = engine.nullBackend.LastBlock();
        for (std::uint32_t sample = 0; sample < kBlock * engine.nullBackend.Channels(); ++sample)
            peak = std::max(peak, std::abs(block[sample]));
    }
    return peak;
}
}

TEST(TimelineVoiceInstall, OriginalSourceActualTwoPlayPublicationCompletesBothBodiesAndFreshPcm) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    std::array<harken::ActionStepProgramViewV1, 2> steps{};
    ASSERT_NO_FATAL_FAILURE(InstallTwoSourceAction(engine, image, steps));
    ASSERT_EQ(VN_OK, PostSourceAction());
    const auto tokens = PublishedSourceTokens(engine);
    ASSERT_EQ(2u, tokens.size());
    for (const auto token : tokens) {
        ASSERT_NE(nullptr, engine.originalSourceSubmissions.Find(token));
        EXPECT_EQ(SourceRegistry::Phase::Published, engine.originalSourceSubmissions.Find(token)->State());
        EXPECT_FALSE(engine.timelineArms.AcknowledgeApplied(token, 99));
    }
    const auto completion = engine.mixer.BlocksRendered() + 1u;
    engine.nullBackend.Pump(1);
    for (const auto token : tokens) {
        EXPECT_EQ(SourceRegistry::Phase::Disposed, engine.originalSourceSubmissions.Find(token)->State());
        EXPECT_EQ(completion, engine.timelineArms.RowAt(token.slot)->armedCompletionSequence.load());
        EXPECT_FALSE(engine.timelineArms.RowAt(token.slot)->originalLifetimePinned.load());
    }
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    EXPECT_GT(FreshSourcePeak(engine), 0.4f);
    EXPECT_FALSE(engine.runtimeRecreateRequired);
}

TEST(TimelineVoiceInstall, SingleSourcePacedAndBurstKeepInstallationAndFirstOutputBoundary) {
    constexpr std::uint32_t frames = 256;
    std::array<float, frames * 2u> firstOutput{};
    std::uint32_t firstOutputDelay = 0;
    for (const bool paced : {false, true}) {
        for (const auto count : {1u, 64u, 128u, 256u}) {
            SCOPED_TRACE(::testing::Message() << "paced=" << paced << " count=" << count);
            EngineSession session(TimelineInstallConfig(256, frames, kRate));
            ASSERT_EQ(VN_OK, session.InitResult());
            auto& engine = *session.Engine();
            const auto samples = MakeDC((count + 1u) * 8u * frames, 0.001f);
            const auto pcm = MonoDesc(samples, kRate);
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &pcm));
            DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
            ASSERT_EQ(harken::WorldInstallStatus::Ok,
                harken::InstallContainerWorldV1(engine, image.world).status);
            VnEmitterHandle emitter = 0;
            ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
            VnPostActionDesc post{};
            post.structSize = sizeof(post);
            post.actionId = kAction;
            post.emitter = emitter;
            const auto before = engine.mixer.BlocksRendered();
            const auto checkBlock = [&](std::uint32_t playing) {
                const auto tokens = PublishedSourceTokens(engine);
                EXPECT_EQ(paced ? 1u : playing, tokens.size());
                const auto completion = engine.mixer.BlocksRendered() + 1u;
                const auto pump = [&] {
                    EXPECT_EQ(VN_OK, Vn_Update());
                    engine.nullBackend.Pump(1);
                };
                pump();
                for (const auto token : tokens) {
                    EXPECT_EQ(completion,
                        engine.timelineArms.RowAt(token.slot)->armedCompletionSequence.load());
                    EXPECT_EQ(SourceRegistry::Phase::Disposed,
                        engine.originalSourceSubmissions.Find(token)->State());
                }
                std::uint32_t outputDelay = UINT32_MAX;
                for (std::uint32_t step = 0; step < 7u; ++step) {
                    const auto* block = engine.nullBackend.LastBlock();
                    const bool audible = std::any_of(block, block + frames * 2u,
                        [](float sample) { return sample != 0.0f; });
                    if (audible && outputDelay == UINT32_MAX) {
                        outputDelay = step;
                        if (!paced) {
                            if (count == 1u) {
                                firstOutputDelay = step;
                                std::copy_n(block, firstOutput.size(), firstOutput.data());
                            }
                            EXPECT_EQ(firstOutputDelay, step);
                            float error = 0.0f;
                            for (std::uint32_t i = 0; i < firstOutput.size(); ++i)
                                error = std::max(error, std::abs(count * firstOutput[i] - block[i]));
                            EXPECT_LT(error, 0.000002f);
                        }
                    }
                    if (step != 6u) pump();
                }
                EXPECT_NE(UINT32_MAX, outputDelay);
                EXPECT_EQ(playing, engine.voices.ActiveCount());
                const auto* block = engine.nullBackend.LastBlock();
                float maximumError = 0.0f;
                for (std::uint32_t i = 0; i < frames * engine.nullBackend.Channels(); ++i)
                    maximumError = std::max(maximumError,
                        std::abs(playing * 0.001f - block[i]));
                EXPECT_LT(maximumError, 0.000002f);
                EXPECT_FALSE(engine.runtimeRecreateRequired);
            };
            for (std::uint32_t i = 0; i < count; ++i) {
                VnActionFiringHandle firing = 0;
                ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing)) << "source=" << i;
                ASSERT_NE(0u, firing);
                if (paced) checkBlock(i + 1u);
            }
            if (!paced) {
                EXPECT_EQ(before, engine.mixer.BlocksRendered());
                checkBlock(count);
            }
        }
    }
}

TEST(TimelineVoiceInstall, OriginalSourceBusyRealRowLeavesWholeQueueAndAcknowledgesFreshBlock) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_EQ(VN_OK, PostSourceAction());
    const auto tokens = PublishedSourceTokens(engine);
    ASSERT_EQ(1u, tokens.size());
    auto* row = const_cast<harken::TimelineArmTable::Row*>(engine.timelineArms.RowAt(tokens[0].slot));
    ASSERT_FALSE(row->reservation.test_and_set(std::memory_order_acquire));
    const auto tail = engine.commands.ConsumedCount();
    engine.nullBackend.Pump(1);
    EXPECT_EQ(tail, engine.commands.ConsumedCount());
    EXPECT_EQ(0u, row->armedCompletionSequence.load());
    EXPECT_TRUE(row->originalLifetimePinned.load());
    row->reservation.clear(std::memory_order_release);
    const auto actualCompletion = engine.mixer.BlocksRendered() + 1u;
    engine.nullBackend.Pump(1);
    EXPECT_EQ(actualCompletion, row->armedCompletionSequence.load());
    EXPECT_EQ(SourceRegistry::Phase::Disposed, engine.originalSourceSubmissions.Find(tokens[0])->State());
    EXPECT_GT(FreshSourcePeak(engine), 0.2f);
}

TEST(TimelineVoiceInstall, OriginalSourceMaximumBlockRefusesBeforeActualPop) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_EQ(VN_OK, PostSourceAction());
    const auto tokens = PublishedSourceTokens(engine);
    ASSERT_EQ(1u, tokens.size());
    const auto tail = engine.commands.ConsumedCount();
    EXPECT_NE(0u, engine.mixer.DrainCommandsForTest(UINT64_MAX));
    EXPECT_EQ(tail, engine.commands.ConsumedCount());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(tokens[0].slot)->armedCompletionSequence.load());
    EXPECT_EQ(SourceRegistry::Phase::Published, engine.originalSourceSubmissions.Find(tokens[0])->State());
    EXPECT_GT(FreshSourcePeak(engine), 0.2f);
}

TEST(TimelineVoiceInstall, OriginalSourceCancelDisposesOriginalAndKeepsSiblingFreshPcm) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_EQ(VN_OK, PostSourceAction());
    ASSERT_EQ(VN_OK, PostSourceAction());
    const auto tokens = PublishedSourceTokens(engine);
    ASSERT_EQ(2u, tokens.size());
    harken::VoiceReleaseObserver firstRelease{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(SourceRegistry::RenderOf(tokens[0]), &firstRelease));
    ASSERT_TRUE(engine.timelineArms.RequestCancel(tokens[0]));
    engine.nullBackend.Pump(1);
    EXPECT_TRUE(engine.voices.ReleaseObserved(firstRelease));
    EXPECT_EQ(0u, engine.timelineArms.RowAt(tokens[0].slot)->armedCompletionSequence.load());
    EXPECT_EQ(harken::ArmState::Tombstoned, engine.timelineArms.RowAt(tokens[0].slot)->state.load());
    EXPECT_NE(0u, engine.timelineArms.RowAt(tokens[1].slot)->armedCompletionSequence.load());
    for (const auto token : tokens)
        EXPECT_EQ(SourceRegistry::Phase::Disposed, engine.originalSourceSubmissions.Find(token)->State());
    EXPECT_GT(FreshSourcePeak(engine), 0.2f);
}

TEST(TimelineVoiceInstall, OriginalSourceCorruptWireCountUsesOriginalRangeAndKeepsSibling) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolume(0.0f));
    ASSERT_EQ(VN_OK, PostSourceAction());
    ASSERT_EQ(VN_OK, PostSourceAction());
    const auto tokens = PublishedSourceTokens(engine);
    ASSERT_EQ(2u, tokens.size());
    harken::CommandRecord wire{};
    ASSERT_TRUE(engine.commands.Peek(wire));
    ASSERT_EQ(harken::kOpcodePlay, wire.opcode);
    wire.payload.play.batch.batchCommandCount = engine.commands.Capacity();
    harken::detail::CommandQueueSubmissionTestAccess::OverwriteFront(engine.commands, wire);
    const auto violations = engine.mixer.CommandProtocolViolationCount();
    engine.nullBackend.Pump(1);
    EXPECT_EQ(violations + 1u, engine.mixer.CommandProtocolViolationCount());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(tokens[0].slot)->armedCompletionSequence.load());
    EXPECT_NE(0u, engine.timelineArms.RowAt(tokens[1].slot)->armedCompletionSequence.load());
    EXPECT_EQ(engine.commands.PublishedHead(), engine.commands.ConsumedCount());
    EXPECT_GT(FreshSourcePeak(engine), 0.2f);
}

TEST(TimelineVoiceInstall, OriginalSourceTerminationWithdrawsPublishedLifetimeLoansAfterJoin) {
    {
        EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
        DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
        ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
        ASSERT_EQ(VN_OK, PostSourceAction());
        const auto tokens = PublishedSourceTokens(engine);
        ASSERT_EQ(1u, tokens.size());
        EXPECT_EQ(SourceRegistry::Phase::Published, engine.originalSourceSubmissions.Find(tokens[0])->State());
    }
    EngineSession restarted(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, restarted.InitResult());
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(restarted.Engine()->voices));
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(restarted.Engine()->registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(*restarted.Engine(), image.world).status);
    ASSERT_EQ(VN_OK, PostSourceAction());
    EXPECT_GT(FreshSourcePeak(*restarted.Engine()), 0.2f);
}

TEST(TimelineVoiceInstall, OriginalSourceUnpublishedRollbackDoesNotCancelSameHandleReplacement) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    struct Replacement { VnVoiceHandle old = 0, fresh = 0; harken::VoiceReleaseObserver release{}; } replacement;
    engine.timelineVoicePreparedContextForTest = &replacement;
    engine.timelineVoicePreparedHookForTest = [](harken::Engine& current,
        const harken::EnginePreparedTimelineVoiceStart& prepared, void* context) {
        auto& replaced = *static_cast<Replacement*>(context);
        replaced.old = prepared.renderHandle;
        EXPECT_TRUE(current.voices.CancelPreparedVoice(prepared.renderHandle));
        const auto pcm = current.registry.Lookup(kSound);
        EXPECT_EQ(VN_OK, current.voices.Play(pcm, 0, 1, 0, false, 0, 0, 0,
            harken::ResolvePanGains(current.config.mixChannelLayout, 0), 0, pcm.frames, 1,
            harken::ResampleProfile::LegacyCompatibility, &replaced.fresh, 1,
            static_cast<std::uint32_t>(replaced.old), 0, true));
        EXPECT_EQ(replaced.old, replaced.fresh);
        EXPECT_TRUE(current.voices.CaptureReleaseObserver(replaced.fresh, &replaced.release));
    };
    EXPECT_NE(VN_OK, PostSourceAction());
    engine.timelineVoicePreparedHookForTest = nullptr;
    ASSERT_NE(0u, replacement.fresh);
    ASSERT_EQ(replacement.old, replacement.fresh);
    EXPECT_FALSE(engine.voices.ReleaseObserved(replacement.release));
    ASSERT_TRUE(engine.voices.CommitPreparedVoice(replacement.fresh));
    engine.voices.ApplyPlay(replacement.fresh, replacement.fresh, 0, engine.mixer.BlocksRendered());
    EXPECT_GT(FreshSourcePeak(engine), 0.2f);
    EXPECT_FALSE(engine.voices.ReleaseObserved(replacement.release));
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    engine.voices.ApplyStop(replacement.fresh, 0, 0, engine.notifications, engine.mixer.BlocksRendered());
}

TEST(TimelineVoiceInstall, OriginalSourceCancelReleaseHeldAcrossBlocksUsesFinalDispositionFence) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_EQ(VN_OK, PostSourceAction());
    const auto tokens = PublishedSourceTokens(engine);
    ASSERT_EQ(1u, tokens.size());
    const auto token = tokens[0];
    const auto service = std::find_if(engine.playableServices.begin(), engine.playableServices.end(),
        [&](const auto& row) { return row.renderHandle == SourceRegistry::RenderOf(token); });
    ASSERT_NE(engine.playableServices.end(), service);
    ASSERT_TRUE(service->usesCapturedVoice);
    ASSERT_TRUE(service->capturedVoice.Valid());
    harken::VoiceReleaseObserver release{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(SourceRegistry::RenderOf(token), &release));
    ASSERT_TRUE(engine.timelineArms.RequestCancel(token));
    struct HeldContext {
        harken::Engine* engine;
        harken::TimelineArmToken token;
        harken::VoiceReleaseObserver release;
        std::uint64_t tail;
        const harken::EnginePlayableServiceRow* service;
        const harken::TimelineVoiceInstallFacts* facts;
        std::uint32_t calls = 0;
    } held{&engine, token, release, engine.commands.ConsumedCount(),
        &*service, service->capturedVoice.Input().facts};
    harken::detail::VoicePoolReleaseTestAccess::SetReleaseHeldHook(engine.voices, [](void* opaque) {
        auto& context = *static_cast<HeldContext*>(opaque);
        ++context.calls;
        const auto before = context.engine->mixer.BlocksRendered();
        for (std::uint32_t i = 0; i < 2; ++i) {
            context.engine->nullBackend.Pump(1);
            EXPECT_EQ(before + i + 1u, context.engine->mixer.BlocksRendered());
            EXPECT_EQ(context.tail, context.engine->commands.ConsumedCount());
            EXPECT_FALSE(context.engine->voices.ReleaseObserved(context.release));
            const auto render = SourceRegistry::RenderOf(context.token);
            EXPECT_EQ(nullptr, context.engine->voices.Resolve(render));
            EXPECT_TRUE(context.engine->voices.IsAlive(render));
            EXPECT_EQ(VN_OK, context.engine->voices.CheckLive(render));
            ASSERT_TRUE(context.service->capturedVoice.Valid());
            EXPECT_EQ(context.facts, context.service->capturedVoice.Input().facts);
            const auto* row = context.engine->timelineArms.RowAt(context.token.slot);
            EXPECT_EQ(0u, row->armedCompletionSequence.load());
            EXPECT_EQ(0u, row->tombstoneAudioSequence.load());
        }
    }, &held);
    ASSERT_TRUE(engine.voices.AbortClaimedVoice(SourceRegistry::RenderOf(token)));
    ASSERT_EQ(1u, held.calls);
    ASSERT_TRUE(engine.voices.ReleaseObserved(release));
    EXPECT_FALSE(engine.voices.IsAlive(SourceRegistry::RenderOf(token)));
    const auto finalSequence = engine.mixer.BlocksRendered() + 1u;
    engine.nullBackend.Pump(1);
    const auto* row = engine.timelineArms.RowAt(token.slot);
    EXPECT_EQ(0u, row->armedCompletionSequence.load());
    EXPECT_EQ(finalSequence, row->tombstoneAudioSequence.load());
    EXPECT_EQ(SourceRegistry::Phase::Disposed, engine.originalSourceSubmissions.Find(token)->State());
    EXPECT_EQ(engine.commands.PublishedHead(), engine.commands.ConsumedCount());
    EXPECT_FALSE(engine.timelineArms.Reclaim(token, finalSequence));
    engine.nullBackend.Pump(1);
    ASSERT_GT(engine.mixer.BlocksRendered(), finalSequence);
    EXPECT_TRUE(engine.timelineArms.Reclaim(token, engine.mixer.BlocksRendered()));
}

TEST(TimelineVoiceInstall, OriginalSourceUnpublishedRollbackPreservesActualReplacementBindingsAndMirror) {
    EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_NO_FATAL_FAILURE(AttachVoiceVolume(-6.0f));
    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    const auto cut = ResolveSoundCut(&storage, engine.registry.Lookup(kSound).contentRevision);
    struct Replacement {
        harken::TimelineVoiceInstallDesc install{};
        VnVoiceHandle old = 0, fresh = 0;
        harken::EngineTimelineVoiceInstallIdentity identity{};
        harken::EngineTimelineVoiceInstallIdentity independentOwner{};
        harken::ProgramGeneration programGeneration = 0;
    } replacement;
    ASSERT_EQ(VN_OK, PostSourceAction());
    const auto independentTokens = PublishedSourceTokens(engine);
    ASSERT_EQ(1u, independentTokens.size());
    ASSERT_TRUE(engine.CaptureTimelineVoiceInstallIdentity(SourceRegistry::RenderOf(independentTokens[0]),
        &replacement.independentOwner));
    ASSERT_NE(0u, replacement.independentOwner.rootInstance);
    replacement.programGeneration = engine.timelineArms.RowAt(independentTokens[0].slot)->programGeneration;
    ASSERT_EQ(harken::PlayableStartStatus::Accepted,
        harken::test::BuildTimelineVoiceInstallFromCut(engine, cut, &replacement.install));
    ASSERT_EQ(1u, replacement.install.bindingCount);
    VnBindingAttachDesc detach{};
    detach.structSize = sizeof(detach);
    detach.scopeKind = VN_TARGET_SCOPE_VOICE;
    detach.objectId = kSound;
    ASSERT_EQ(VN_OK, Vn_DetachBinding(kBinding, &detach));
    engine.timelineVoicePreparedContextForTest = &replacement;
    engine.timelineVoicePreparedHookForTest = [](harken::Engine& current,
        const harken::EnginePreparedTimelineVoiceStart& prepared, void* opaque) {
        auto& replaced = *static_cast<Replacement*>(opaque);
        current.timelineVoicePreparedHookForTest = nullptr;
        replaced.old = prepared.renderHandle;
        EXPECT_NE(replaced.independentOwner.rootInstance, prepared.ledgerPreparation.parent.internalToken);
        EXPECT_EQ(0u, BindingInstanceCount(&current, replaced.old));
        EXPECT_TRUE(current.voices.CancelPreparedVoice(prepared.renderHandle));
        const auto installed = harken::test::InstallTimelineVoiceStart(current, replaced.install,
            replaced.programGeneration, replaced.independentOwner.rootInstance,
            replaced.independentOwner.rootInstance);
        EXPECT_EQ(VN_OK, installed.status);
        replaced.fresh = installed.renderHandle;
        EXPECT_EQ(replaced.old, replaced.fresh);
        EXPECT_TRUE(current.CaptureTimelineVoiceInstallIdentity(replaced.fresh, &replaced.identity));
        EXPECT_EQ(1u, BindingInstanceCount(&current, replaced.fresh));
    };
    EXPECT_NE(VN_OK, PostSourceAction());
    engine.timelineVoicePreparedHookForTest = nullptr;
    ASSERT_NE(0u, replacement.fresh);
    ASSERT_EQ(replacement.old, replacement.fresh);
    EXPECT_FALSE(engine.voices.ReleaseObserved(replacement.identity.releaseObserver));
    EXPECT_EQ(1u, BindingInstanceCount(&engine, replacement.fresh));
    float contribution = 0;
    EXPECT_TRUE(BindingContribution(&engine, replacement.fresh, &contribution));
    harken::EngineTimelineVoiceInstallIdentity retained{};
    ASSERT_TRUE(engine.CaptureTimelineVoiceInstallIdentity(replacement.fresh, &retained));
    EXPECT_EQ(replacement.identity.releaseObserver.slot, retained.releaseObserver.slot);
    EXPECT_EQ(replacement.identity.releaseObserver.targetSequence, retained.releaseObserver.targetSequence);
    EXPECT_TRUE(SourceRegistry::SameToken(replacement.identity.startArm, retained.startArm));
    const auto completion = engine.mixer.BlocksRendered() + 1u;
    engine.nullBackend.Pump(1);
    EXPECT_EQ(completion, engine.timelineArms.RowAt(retained.startArm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(1u, BindingInstanceCount(&engine, replacement.fresh));
    EXPECT_FALSE(engine.voices.ReleaseObserved(retained.releaseObserver));
}

TEST(TimelineVoiceInstall, EffectMirrorPublishesAuthoredConfigOnlyAfterValidatedCommit) {
    // Plain installation, ordered metadata in two children, malformed payload,
    // a later commit refusal, and unsupported prepared compressor metadata.
    for (const auto mode : {0u, 1u, 2u, 3u, 4u}) {
        SCOPED_TRACE(mode);
        EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
        DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
        std::array<harken::EffectParamV1, 2> params{{
            {harken::kLimiterParamCeilingDb, std::bit_cast<std::uint32_t>(-9.0f)},
            {harken::kLimiterParamReleaseMs, std::bit_cast<std::uint32_t>(73.0f)}}};
        harken::EffectNodeV1 effect{1, VN_EFFECT_LIMITER, 2, params.data(), harkentest::ObjectId(901)};
        image.property.payload.presentMask |= UINT64_C(0x8000);
        image.property.payload.effectNodeCount = 1;
        image.property.payload.effectNodes = &effect;
        image.property.programSemanticFingerprint = harken::ComputePlayablePropertyFingerprintV1(image.property);
        image.step.target.expectedProgramSemanticFingerprint = image.property.programSemanticFingerprint;
        std::array<harken::ActionStepProgramViewV1, 2> steps{};
        if (mode == 1) {
            ASSERT_NO_FATAL_FAILURE(InstallTwoSourceAction(engine, image, steps));
        } else {
            image.action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(image.action);
            image.world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(image.world);
            ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
        }
        ASSERT_TRUE(engine.PrepareLogicVoiceStorage(2));
        for (std::uint32_t row = 0; row < 2; ++row) {
            engine.effectSlotTypes[row].fill(VN_EFFECT_GAIN);
            for (std::uint32_t slot = 0; slot < harken::kMaxFxSlotsPerVoice; ++slot) {
                engine.voiceEffectInstanceRefs[row][slot] = {VN_EFFECT_GAIN, 99, 17};
                engine.effectSlotConfig[row][slot].bypass = true;
                engine.effectSlotConfig[row][slot].params.fill(23.0f);
            }
        }
        struct Context {
            std::uint32_t mode, count = 0;
            std::array<VnVoiceHandle, 2> renders{};
        } context{mode};
        engine.timelineVoicePreparedContextForTest = &context;
        engine.timelineVoicePreparedHookForTest = [](harken::Engine& current,
            const harken::EnginePreparedTimelineVoiceStart& captured, void* opaque) {
            auto& context = *static_cast<Context*>(opaque);
            const auto ordinal = context.count++;
            ASSERT_LT(ordinal, context.renders.size());
            context.renders[ordinal] = captured.renderHandle;
            const auto row = static_cast<std::uint32_t>(captured.renderHandle);
            for (std::uint32_t slot = 0; slot < harken::kMaxFxSlotsPerVoice; ++slot) {
                EXPECT_EQ(VN_EFFECT_GAIN, current.effectSlotTypes[row][slot]);
                EXPECT_EQ(17u, current.voiceEffectInstanceRefs[row][slot].generation);
                EXPECT_TRUE(current.effectSlotConfig[row][slot].bypass);
                EXPECT_FLOAT_EQ(23.0f, current.effectSlotConfig[row][slot].params[0]);
            }
            auto& prepared = const_cast<harken::EnginePreparedTimelineVoiceStart&>(captured);
            auto found = std::find_if(prepared.commands.begin(), prepared.commands.begin() + prepared.commandCount,
                [](const auto& command) { return command.opcode == harken::kOpcodeSetVoiceEffect; });
            ASSERT_NE(prepared.commands.begin() + prepared.commandCount, found);
            EXPECT_FLOAT_EQ(-9.0f, found->payload.setVoiceEffect.initialParams[0]);
            EXPECT_FLOAT_EQ(73.0f, found->payload.setVoiceEffect.initialParams[1]);
            if (context.mode == 1) {
                ASSERT_GE(found - prepared.commands.begin(), 3);
                const auto install = *found;
                prepared.commands[1] = {};
                prepared.commands[1].opcode = harken::kOpcodeSetVoiceEffectBypass;
                prepared.commands[1].payload.setVoiceEffectBypass.handle = captured.renderHandle;
                prepared.commands[1].payload.setVoiceEffectBypass.bypass = 1;
                prepared.commands[2] = install;
                *found = {};
                found->opcode = harken::kOpcodeSetVoiceEffectParam;
                auto& param = found->payload.setVoiceEffectParam;
                param.handle = captured.renderHandle;
                param.effectType = VN_EFFECT_LIMITER;
                param.paramIndex = harken::kLimiterParamCeilingDb;
                param.value = -18.0f - ordinal * 3.0f;
                ASSERT_TRUE(harken::PrepareLimiterParam(param.paramIndex, param.value, kRate, &param.coeffs.b0));
            } else if (context.mode == 2) {
                found->payload.setVoiceEffect.initialParams[0] = std::numeric_limits<float>::quiet_NaN();
            } else if (context.mode == 3) {
                current.forceTimelineVoiceCommitRollbackForTest = true;
            } else if (context.mode == 4) {
                prepared.commands[1] = {};
                prepared.commands[1].opcode = harken::kOpcodeSetVoiceEffectParam;
                prepared.commands[1].flags = harken::kCommandFlagPreparedCompressor;
                prepared.commands[1].payload.setCompressorEffectParam.host = captured.renderHandle;
            }
        };
        const auto published = engine.commands.PublishedHead();
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        VnPostActionDesc post{};
        post.structSize = sizeof(post);
        post.actionId = kAction;
        post.emitter = emitter;
        VnActionFiringHandle firing = 0;
        const auto posted = Vn_PostAction(&post, &firing);
        engine.timelineVoicePreparedHookForTest = nullptr;
        ASSERT_EQ(mode == 1 ? 2u : 1u, context.count);
        if (mode >= 2) {
            if (mode != 3) EXPECT_NE(VN_OK, posted);
            else {
                ASSERT_EQ(VN_OK, posted);
                ASSERT_NE(0u, firing);
                EXPECT_FALSE(engine.forceTimelineVoiceCommitRollbackForTest);
                EXPECT_TRUE(engine.runtimeRecreateRequired);
                VnActionStepResult step{};
                step.structSize = sizeof(step);
                ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
                EXPECT_EQ(VN_ACTION_STEP_FAILED, step.state);
                EXPECT_EQ(VN_RUNTIME_RECREATE_REQUIRED, step.result);
            }
            EXPECT_EQ(published, engine.commands.PublishedHead());
            EXPECT_TRUE(PublishedSourceTokens(engine).empty());
            EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
        } else ASSERT_EQ(VN_OK, posted);
        for (std::uint32_t ordinal = 0; ordinal < context.count; ++ordinal) {
            const auto row = static_cast<std::uint32_t>(context.renders[ordinal]);
            for (std::uint32_t slot = 0; slot < harken::kMaxFxSlotsPerVoice; ++slot) {
                const auto& config = engine.effectSlotConfig[row][slot];
                if (mode >= 2) {
                    EXPECT_EQ(VN_EFFECT_GAIN, engine.effectSlotTypes[row][slot]);
                    EXPECT_EQ(17u, engine.voiceEffectInstanceRefs[row][slot].generation);
                    EXPECT_TRUE(config.bypass);
                    EXPECT_FLOAT_EQ(23.0f, config.params[0]);
                } else if (slot == 0) {
                    EXPECT_EQ(VN_EFFECT_LIMITER, engine.effectSlotTypes[row][slot]);
                    const auto ref = engine.voiceEffectInstanceRefs[row][slot];
                    EXPECT_NE(nullptr, engine.effectBank.LiveStateFor(ref));
                    EXPECT_EQ(effect.effectId, engine.effectBank.ObjectIdentity(ref).effectId);
                    EXPECT_EQ(mode == 1, config.bypass);
                    EXPECT_FLOAT_EQ(mode == 1 ? -18.0f - ordinal * 3.0f : -9.0f, config.params[0]);
                    EXPECT_FLOAT_EQ(73.0f, config.params[1]);
                } else {
                    EXPECT_EQ(harken::kEffectTypeNone, engine.effectSlotTypes[row][slot]);
                    EXPECT_EQ(harken::kEffectTypeNone, engine.voiceEffectInstanceRefs[row][slot].type);
                    EXPECT_FALSE(config.bypass);
                    EXPECT_EQ((std::array<float, harken::kMaxEffectParamCount>{}), config.params);
                }
            }
        }
    }
}

TEST(TimelineVoiceInstall, EmptyInitialSourceClearsAllEightMirrorsAfterDirectVoiceReuse) {
    EngineSession session(TimelineInstallConfig(1, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    const auto play = harkentest::MakePlayDesc(kSound);
    VnVoiceHandle direct = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &direct));
    const auto oldRender = engine.ledger.Classify(direct).renderHandle;
    const auto row = static_cast<std::uint32_t>(oldRender);
    VnLimiterDesc limiter{};
    limiter.structSize = sizeof(limiter);
    limiter.ceilingDb = -11.0f;
    limiter.releaseMs = 89.0f;
    for (std::uint32_t slot = 0; slot < harken::kMaxFxSlotsPerVoice; ++slot) {
        ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(direct, slot, VN_EFFECT_LIMITER,
            reinterpret_cast<const VnEffectDesc*>(&limiter)));
        ASSERT_EQ(VN_OK, Vn_SetVoiceEffectBypass(direct, slot, 1));
        EXPECT_FLOAT_EQ(-11.0f, engine.effectSlotConfig[row][slot].params[0]);
        EXPECT_TRUE(engine.effectSlotConfig[row][slot].bypass);
    }
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Stop(direct, 0));
    engine.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    for (std::uint32_t slot = 0; slot < harken::kMaxFxSlotsPerVoice; ++slot) {
        ASSERT_EQ(VN_EFFECT_LIMITER, engine.effectSlotTypes[row][slot]);
        ASSERT_FLOAT_EQ(-11.0f, engine.effectSlotConfig[row][slot].params[0]);
        ASSERT_TRUE(engine.effectSlotConfig[row][slot].bypass);
    }
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_EQ(VN_OK, PostSourceAction());
    const auto tokens = PublishedSourceTokens(engine);
    ASSERT_EQ(1u, tokens.size());
    const auto fresh = SourceRegistry::RenderOf(tokens.front());
    EXPECT_EQ(row, static_cast<std::uint32_t>(fresh));
    EXPECT_NE(oldRender, fresh);
    for (std::uint32_t slot = 0; slot < harken::kMaxFxSlotsPerVoice; ++slot) {
        EXPECT_EQ(harken::kEffectTypeNone, engine.effectSlotTypes[row][slot]);
        EXPECT_EQ(harken::kEffectTypeNone, engine.voiceEffectInstanceRefs[row][slot].type);
        EXPECT_FALSE(engine.effectSlotConfig[row][slot].bypass);
        EXPECT_EQ((std::array<float, harken::kMaxEffectParamCount>{}), engine.effectSlotConfig[row][slot].params);
    }
    EXPECT_GT(FreshSourcePeak(engine), 0.2f);
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
}

TEST(TimelineVoiceInstall, LiveEffectChainIsAtomicPreservesStateAndRetiresAcrossWorkers) {
    for (auto workers : {0u, 2u}) {
        SCOPED_TRACE(workers);
        auto configuration = TimelineInstallConfig(8, kBlock, kRate);
        configuration.renderWorkerCount = workers;
        EngineSession session(configuration);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
        DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
        ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
        ASSERT_EQ(VN_OK, PostSourceAction());
        const auto tokens = PublishedSourceTokens(engine);
        ASSERT_EQ(1u, tokens.size());
        const auto render = SourceRegistry::RenderOf(tokens[0]);
        ASSERT_GT(FreshSourcePeak(engine), 0.2f);
        struct Observed { VnVoiceHandle handle; bool found = false; } observed{render};
        ASSERT_TRUE(harken::ObserveCurrentPlaybackPaths(
            [](const harken::PlaybackPathObservation& path, void* opaque) {
                auto& result = *static_cast<Observed*>(opaque);
                result.found = result.found || path.voiceInstance == result.handle;
            }, &observed));
        ASSERT_TRUE(observed.found);
        const auto row = static_cast<std::uint32_t>(render);
        auto* context = engine.voices.Resolve(render);
        ASSERT_NE(nullptr, context);
        VnPreviewVoiceEffectChainV1 chain{};
        chain.structSize = sizeof(chain);
        chain.entryCount = 2;
        chain.entries[0] = {harkentest::ObjectId(901), VN_EFFECT_LIMITER, 0, -12.0f, 73.0f};
        chain.entries[1] = {harkentest::ObjectId(902), VN_EFFECT_LIMITER, 0, -18.0f, 91.0f};
        ASSERT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(render, &chain));
        const auto original = engine.voiceEffectInstanceRefs[row];
        EXPECT_EQ(2u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
        EXPECT_EQ(harken::kEffectTypeNone, context->fxSlotEffectRef[0].type);
        EXPECT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(render, &chain));
        chain.entries[0].ceilingDb = -6;
        EXPECT_EQ(VN_BACKPRESSURE, VnPreview_SetVoiceEffectChain(render, &chain));
        EXPECT_FLOAT_EQ(-12, engine.effectSlotConfig[row][0].params[0]);
        chain.entries[0].ceilingDb = -12;
        engine.nullBackend.Pump(1);
        ASSERT_NE(nullptr, context->fxChainTransition);
        EXPECT_EQ(64u, context->fxChainTransition->done);
        EXPECT_EQ(240u, context->fxChainTransition->totalFrames);
        EXPECT_EQ(harken::LimiterGainReductionStatus::ChainTransition,
            static_cast<harken::LimiterState*>(context->fxSlots[0].state)->gainReduction.status);
        engine.nullBackend.Pump(3);
        EXPECT_EQ(nullptr, context->fxChainTransition);
        ASSERT_EQ(VN_OK, Vn_Update());
        auto* state = static_cast<harken::LimiterState*>(context->fxSlots[0].state);
        const auto gain = state->currentGain;
        chain.entries[0].ceilingDb = -6;
        ASSERT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(render, &chain));
        EXPECT_EQ(original[0].poolIndex, engine.voiceEffectInstanceRefs[row][0].poolIndex);
        EXPECT_EQ(original[0].generation, engine.voiceEffectInstanceRefs[row][0].generation);
        EXPECT_DOUBLE_EQ(gain, state->currentGain);
        engine.nullBackend.Pump(1);
        EXPECT_EQ(state, context->fxSlots[0].state);
        ASSERT_EQ(VN_OK, Vn_Update());
        std::swap(chain.entries[0], chain.entries[1]);
        ASSERT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(render, &chain));
        const auto moved = engine.voiceEffectInstanceRefs[row];
        EXPECT_NE(original[0].poolIndex, moved[1].poolIndex);
        EXPECT_NE(original[1].poolIndex, moved[0].poolIndex);
        EXPECT_NE(moved[0].poolIndex, moved[1].poolIndex);
        EXPECT_EQ(4u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
        engine.nullBackend.Pump(4);
        EXPECT_EQ(4u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(2u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
        chain.entryCount = 0;
        ASSERT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(render, &chain));
        engine.nullBackend.Pump(1);
        ASSERT_NE(nullptr, context->fxChainTransition);
        EXPECT_EQ(64u, context->fxChainTransition->done);
        engine.nullBackend.Pump(3);
        EXPECT_EQ(nullptr, context->fxChainTransition);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
        EXPECT_GT(FreshSourcePeak(engine), 0.2f);
    }
}

TEST(TimelineVoiceInstall, LiveEffectChainRejectsInvalidAndRollsBackCapacityWithoutPublishing) {
    auto configuration = TimelineInstallConfig(8, kBlock, kRate);
    configuration.limiterEffectPoolCapacity = 1;
    EngineSession session(configuration);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_EQ(VN_OK, PostSourceAction());
    const auto tokens = PublishedSourceTokens(engine);
    ASSERT_EQ(1u, tokens.size());
    const auto render = SourceRegistry::RenderOf(tokens[0]);
        ASSERT_GT(FreshSourcePeak(engine), 0.2f);
        struct Observed { VnVoiceHandle handle; bool found = false; } observed{render};
        ASSERT_TRUE(harken::ObserveCurrentPlaybackPaths(
            [](const harken::PlaybackPathObservation& path, void* opaque) {
                auto& result = *static_cast<Observed*>(opaque);
                result.found = result.found || path.voiceInstance == result.handle;
            }, &observed));
        ASSERT_TRUE(observed.found);
    VnPreviewVoiceEffectChainV1 chain{};
    chain.structSize = sizeof(chain);
    chain.entryCount = 2;
    chain.entries[0] = {harkentest::ObjectId(901), VN_EFFECT_LIMITER, 0, -12.0f, 73.0f};
    chain.entries[1] = {harkentest::ObjectId(902), VN_EFFECT_LIMITER, 0, -18.0f, 91.0f};
    const auto published = engine.commands.PublishedHead();
    EXPECT_EQ(VN_NO_EFFECT, VnPreview_SetVoiceEffectChain(render, &chain));
    EXPECT_EQ(published, engine.commands.PublishedHead());
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    chain.entries[1].effectId = chain.entries[0].effectId;
    EXPECT_EQ(VN_INVALID_ARG, VnPreview_SetVoiceEffectChain(render, &chain));
    chain.entryCount = 1;
    chain.entries[0].ceilingDb = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(VN_INVALID_ARG, VnPreview_SetVoiceEffectChain(render, &chain));
    chain.entries[0].ceilingDb = -12;
    chain.entries[0].effectType = VN_EFFECT_GAIN;
    EXPECT_EQ(VN_INVALID_ARG, VnPreview_SetVoiceEffectChain(render, &chain));
    chain.entries[0].effectType = VN_EFFECT_LIMITER;
    ASSERT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(render, &chain));
    ASSERT_EQ(VN_OK, Vn_StopAll(0));
    engine.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    EXPECT_EQ(VN_STALE_HANDLE, VnPreview_SetVoiceEffectChain(render, &chain));
}

TEST(TimelineVoiceInstall, LiveEffectChainRetirementBackpressureRetainsOwnerAfterStop) {
    for (const bool stop : {false, true}) {
        SCOPED_TRACE(stop);
        EngineSession session(TimelineInstallConfig(8, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
        DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
        ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
        ASSERT_EQ(VN_OK, PostSourceAction());
        const auto tokens = PublishedSourceTokens(engine);
        ASSERT_EQ(1u, tokens.size());
        const auto render = SourceRegistry::RenderOf(tokens[0]);
        ASSERT_GT(FreshSourcePeak(engine), 0.2f);
        struct Observed { VnVoiceHandle handle; bool found = false; } observed{render};
        ASSERT_TRUE(harken::ObserveCurrentPlaybackPaths(
            [](const harken::PlaybackPathObservation& path, void* opaque) {
                auto& result = *static_cast<Observed*>(opaque);
                result.found = result.found || path.voiceInstance == result.handle;
            }, &observed));
        ASSERT_TRUE(observed.found);
        const auto slot = static_cast<std::uint32_t>(render);
        VnPreviewVoiceEffectChainV1 chain{};
        chain.structSize = sizeof(chain);
        chain.entryCount = 1;
        chain.entries[0] = {harkentest::ObjectId(901), VN_EFFECT_LIMITER, 0, -12.0f, 73.0f};
        ASSERT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(render, &chain));
        engine.nullBackend.Pump(4);
        ASSERT_EQ(VN_OK, Vn_Update());
        const auto old = engine.voiceEffectInstanceRefs[slot][0];
        chain.entries[0].effectId = harkentest::ObjectId(902);
        ASSERT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(render, &chain));
        auto* update = harken::detail::VoicePoolReleaseTestAccess::EffectUpdate(engine.voices, slot);
        ASSERT_NE(nullptr, update);
        auto& lane = harken::detail::EffectPoolTestAccess::RetireLane(engine.effectBank, old);
        std::uint32_t occupied = 0;
        while (lane.TryPush({})) ++occupied;
        ASSERT_GT(occupied, 0u);
        engine.nullBackend.Pump(1);
        if (stop) ASSERT_EQ(VN_OK, Vn_StopAll(0));
        engine.nullBackend.Pump(3);
        EXPECT_EQ(harken::EffectChainUpdate::Phase::Retiring, update->phase.load());
        EXPECT_FALSE(update->scratch.empty());
        EXPECT_EQ(2u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
        if (!stop) {
            chain.entries[0].effectId = harkentest::ObjectId(903);
            EXPECT_EQ(VN_BACKPRESSURE, VnPreview_SetVoiceEffectChain(render, &chain));
        }
        // Setter admission may drain the lane but must not release the row.
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_NE(harken::EffectChainUpdate::Phase::Free, update->phase.load());
        engine.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(harken::EffectChainUpdate::Phase::Free, update->phase.load());
        EXPECT_TRUE(update->scratch.empty());
        EXPECT_EQ(stop ? 0u : 1u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    }
}


TEST(TimelineVoiceInstall, LiveEffectChainEightSlotsUseOneCommandAndPreserveBypassAcrossSuspend) {
    EngineSession session(TimelineInstallConfig(1, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_EQ(VN_OK, PostSourceAction());
    const auto tokens = PublishedSourceTokens(engine);
    ASSERT_EQ(1u, tokens.size());
    const auto render = SourceRegistry::RenderOf(tokens[0]);
    ASSERT_GT(FreshSourcePeak(engine), 0.2f);
    auto* voice = engine.voices.Resolve(render);
    ASSERT_NE(nullptr, voice);
    VnPreviewVoiceEffectChainV1 chain{};
    chain.structSize = sizeof(chain);
    chain.entryCount = 8;
    for (std::uint32_t i = 0; i < 8; ++i)
        chain.entries[i] = {harkentest::ObjectId(901 + i), VN_EFFECT_LIMITER, 0, -6.0f, 73.0f};
    engine.mixer.SetMaxCommandsPerQuantumForTest(1);
    const auto published = engine.commands.PublishedHead();
    ASSERT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(render, &chain));
    EXPECT_EQ(published + 1, engine.commands.PublishedHead());
    const auto allocations = allocation_probe::count;
    allocation_probe::enabled = true;
    engine.nullBackend.Pump(1);
    allocation_probe::enabled = false;
    EXPECT_EQ(allocations, allocation_probe::count);
    ASSERT_NE(nullptr, voice->fxChainTransition);
    EXPECT_EQ(64u, voice->fxChainTransition->done);
    for (const auto& ref : voice->fxSlotEffectRef) EXPECT_EQ(VN_EFFECT_LIMITER, ref.type);
    engine.mixer.SetMaxCommandsPerQuantumForTest(256);
    ASSERT_EQ(VN_OK, Vn_Suspend());
    engine.nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(64u, voice->fxChainTransition->done);
    ASSERT_EQ(VN_OK, Vn_Resume());
    engine.nullBackend.Pump(3);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto ref = voice->fxSlotEffectRef[0];
    const auto* state = voice->fxSlots[0].state;
    chain.entries[0].bypass = 1;
    ASSERT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(render, &chain));
    engine.nullBackend.Pump(1);
    EXPECT_EQ(state, voice->fxSlots[0].state);
    EXPECT_EQ(ref.generation, voice->fxSlotEffectRef[0].generation);
    EXPECT_EQ(ref.poolIndex, voice->fxSlotEffectRef[0].poolIndex);
    EXPECT_FLOAT_EQ(1.0f, voice->fxBypassMix[0]);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_StopAll(0));
    engine.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    ASSERT_EQ(VN_OK, PostSourceAction());
    const auto reusedTokens = PublishedSourceTokens(engine);
    ASSERT_EQ(1u, reusedTokens.size());
    const auto reused = SourceRegistry::RenderOf(reusedTokens[0]);
    EXPECT_EQ(static_cast<std::uint32_t>(render), static_cast<std::uint32_t>(reused));
    EXPECT_NE(render, reused);
    ASSERT_GT(FreshSourcePeak(engine), 0.2f);
    EXPECT_EQ(VN_STALE_HANDLE, VnPreview_SetVoiceEffectChain(render, &chain));
    ASSERT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(reused, &chain));
    engine.nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(8u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
TEST(TimelineVoiceInstall, LiveEffectChainActiveWorkerQuarantineRetainsScratchUntilActualJoin) {
    struct Gate {
        harken::InsertEffect original{};
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
        static void Process(void* opaque, float* const* planes, std::uint32_t channels,
                            std::uint32_t offset, std::uint32_t frames) {
            auto& gate = *static_cast<Gate*>(opaque);
            gate.original.process(gate.original.state, planes, channels, offset, frames);
            gate.entered.store(true, std::memory_order_release);
            while (!gate.release.load(std::memory_order_acquire)) std::this_thread::yield();
        }
        static void Coordinator(void* opaque, std::uint64_t) noexcept {
            auto& gate = *static_cast<Gate*>(opaque);
            const auto until = harken::CpuClockTicks() + 2 * harken::CpuClockTicksPerSecond();
            while (!gate.entered.load(std::memory_order_acquire) && harken::CpuClockTicks() < until)
                std::this_thread::yield();
        }
        static void Claimed(void*, std::uint64_t, std::uint32_t) noexcept {}
    } gate;
    auto config = TimelineInstallConfig(1, kBlock, kRate);
    config.renderWorkerCount = 1;
    EngineSession session(config);
    struct ReleaseBeforeTerm {
        Gate& gate;
        ~ReleaseBeforeTerm() { gate.release.store(true, std::memory_order_release); }
    } releaseBeforeTerm{gate};
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterLeaf());
    DirectWorldSoundPlayAction image(engine.registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(engine, image.world).status);
    ASSERT_EQ(VN_OK, PostSourceAction());
    const auto tokens = PublishedSourceTokens(engine);
    ASSERT_EQ(1u, tokens.size());
    const auto render = SourceRegistry::RenderOf(tokens[0]);
    const auto slot = static_cast<std::uint32_t>(render);
    ASSERT_GT(FreshSourcePeak(engine), 0.2f);
    VnPreviewVoiceEffectChainV1 chain{};
    chain.structSize = sizeof(chain);
    chain.entryCount = 1;
    chain.entries[0] = {harkentest::ObjectId(901), VN_EFFECT_LIMITER, 0, -6.0f, 73.0f};
    ASSERT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(render, &chain));
    engine.nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    auto* voice = engine.voices.Resolve(render);
    ASSERT_NE(nullptr, voice);
    gate.original = voice->fxSlots[0];
    voice->fxSlots[0] = {};
    voice->fxSlots[0].process = &Gate::Process;
    voice->fxSlots[0].state = &gate;
    chain.entries[0].effectId = harkentest::ObjectId(902);
    ASSERT_EQ(VN_OK, VnPreview_SetVoiceEffectChain(render, &chain));
    using Access = harken::detail::VoicePoolReleaseTestAccess;
    Access::SetOriginalSourceWorkerGates(engine.voices, &Gate::Coordinator, &Gate::Claimed, &gate);
    Access::SetWorkerDeadline(engine.voices, harken::CpuClockTicksPerSecond() / 20);
    const auto epoch = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_TRUE(gate.entered.load(std::memory_order_acquire));
    EXPECT_EQ(epoch, engine.mixer.BlocksRendered());
    auto* update = Access::EffectUpdate(engine.voices, slot);
    ASSERT_NE(nullptr, update);
    EXPECT_EQ(harken::EffectChainUpdate::Phase::Active, update->phase.load());
    EXPECT_FALSE(update->scratch.empty());
    ASSERT_EQ(VN_OK, Vn_Update());
    Access::QuarantineOriginalSlot(engine.voices, engine.notifications, slot, epoch);
    EXPECT_TRUE(Access::IsActuallyQuarantined(engine.voices, slot));
    EXPECT_EQ(harken::EffectChainUpdate::Phase::Active, update->phase.load());
    EXPECT_FALSE(update->scratch.empty());
    EXPECT_EQ(2u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    EXPECT_EQ(VN_STALE_HANDLE, VnPreview_SetVoiceEffectChain(render, &chain));
    gate.release.store(true, std::memory_order_release);
    Access::JoinWorkers(engine.voices);
    engine.voices.ShutdownOriginalSourceBodiesAfterQuiescence();
    EXPECT_TRUE(update->scratch.empty());
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
}
#endif
