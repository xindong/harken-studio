#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/render/BuiltinEffects.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace harken::detail {

struct VariableServiceTestAccess {
    static const void* OutcomeStorage(const VariableServiceV1& service) {
        return service.outcomes_.data();
    }
    static std::array<std::size_t, 3> OutcomeCapacity(const VariableServiceV1& service) {
        return {service.outcomes_.size(), service.outcomes_.capacity(), service.freeSlots_.size()};
    }
    static void SeedWorld(VariableServiceV1& service, WorldGeneration world) {
        service.worldGeneration_ = world;
    }
    static void SeedHistoryEligibility(VariableServiceV1& service, int variant) {
        for (auto& row : service.history_) {
            if (!row.used || row.state == VariableServiceV1::RowState::Terminal) continue;
            row.state = VariableServiceV1::RowState::Pending;
            row.world = service.worldGeneration_;
            row.transport = service.transport_.Current();
            row.slice = {};
            row.noPhysicalTargets = false;
            if (variant == 0) row.state = VariableServiceV1::RowState::TargetsClaimed;
            if (variant == 1) row.state = VariableServiceV1::RowState::WaitingFrontier;
            if (variant == 2) row.state = VariableServiceV1::RowState::SlicePrepared;
            if (variant == 3) row.state = VariableServiceV1::RowState::PageCommitted;
            if (variant == 4) row.state = VariableServiceV1::RowState::Applied;
            if (variant == 5) ++row.world;
            if (variant == 6) ++row.transport;
            if (variant == 7) row.slice = {0, 1};
        }
    }
    static bool HasNoPhysicalFrontier(const VariableServiceV1& service) {
        return std::any_of(service.history_.begin(), service.history_.end(), [](const auto& row) {
            return row.used && row.state == VariableServiceV1::RowState::WaitingFrontier &&
                row.noPhysicalTargets && !row.slice.Valid();
        });
    }
    static std::array<std::uint32_t, 2> BankGenerations(
        const VariableServiceV1& service, VnEmitterHandle emitter) {
        return {service.globalBankVersionGeneration_,
                service.emitterBankVersionGenerations_[static_cast<std::uint32_t>(emitter)]};
    }
    static void SeedGlobalBankVersionGeneration(
        VariableServiceV1& service, std::uint32_t generation) {
        service.globalBankVersionGeneration_ = generation;
    }

    static bool SeedEmitterBankVersionGeneration(
        VariableServiceV1& service, VnEmitterHandle emitter,
        std::uint32_t generation) {
        const std::uint32_t slot = static_cast<std::uint32_t>(emitter);
        const std::uint32_t emitterGeneration =
            static_cast<std::uint32_t>(emitter >> 32);
        if (slot >= service.emitterBanks_.size() ||
            service.emitterBanks_[slot].generation != emitterGeneration) {
            return false;
        }
        service.emitterBankVersionGenerations_[slot] = generation;
        return true;
    }
};

} // namespace harken::detail

namespace {

using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MakeVariableDefinition;
using harkentest::MonoDesc;
using CurrentSliceHookPhase =
    harken::MixerLoop::CurrentSliceHookPhaseForTest;

constexpr std::uint32_t kBlockFrames = 64;
constexpr std::uint32_t kSampleRate = 48000;
constexpr VnVariableId kVariable = harkentest::ObjectId(7001);

float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

harken::Curve LinearCurve(float x0, float y0, float x1, float y1) {
    harken::Curve curve;
    const harken::CurvePoint points[] = {{x0, y0}, {x1, y1}};
    const harken::CurveHandlePair handles[] = {
        {points[0].x, points[0].y, points[1].x, points[1].y}};
    EXPECT_TRUE(curve.SetPoints(points, handles, 2));
    return curve;
}

void InstallVariable(harken::Engine* engine, float minimum = 0.0f,
                     float maximum = 1.0f, float defaultValue = 0.0f,
                     std::uint64_t fingerprint = 0x43555252454E54ull) {
    const harken::VariableDefinitionV1 definition = MakeVariableDefinition(
        kVariable, harken::VariableKindV1::Smooth, minimum, maximum,
        defaultValue);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, &definition, 1,
                                                  fingerprint)
                  .status);
}

void RegisterBusVolumeBinding(VnBindingId binding, VnBusId bus,
                              float x0 = 0.0f, float y0 = 0.0f,
                              float x1 = 1.0f, float y1 = -12.0f,
                              std::uint32_t transitionMs = 0) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = kVariable;
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS;
    desc.targetId = harken::kBusTargetBusVolume;
    desc.transitionMs = transitionMs;
    desc.curve.structSize = sizeof(desc.curve);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {x0, y0};
    desc.curve.points[1] = {x1, y1};
    desc.curve.handles[0] =
        harkentest::EndpointHandles(desc.curve.points[0], desc.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(binding, &desc));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = bus;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(binding, &attach));
}

void RegisterVoiceAttachment(VnBindingId binding, VnSoundId sound,
                             float x0 = 0.0f, float y0 = 0.0f,
                             float x1 = 1.0f, float y1 = -12.0f) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = kVariable;
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    desc.targetId = harken::kTargetVoiceVolume;
    desc.curve.structSize = sizeof(desc.curve);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {x0, y0};
    desc.curve.points[1] = {x1, y1};
    desc.curve.handles[0] =
        harkentest::EndpointHandles(desc.curve.points[0], desc.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(binding, &desc));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_VOICE;
    attach.objectId = sound;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(binding, &attach));
}

void RegisterLinearBinding(VnBindingId binding, VnVariableId variable,
                           std::uint32_t scope, std::uint32_t target,
                           float y0, float y1,
                           std::uint32_t transitionMs = 0) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = variable;
    desc.targetScopeKind = scope;
    desc.targetId = target;
    desc.transitionMs = transitionMs;
    desc.curve.structSize = sizeof(desc.curve);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, y0};
    desc.curve.points[1] = {1.0f, y1};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(binding, &desc));
}

VnVoiceHandle PlayDc(harken::Engine* engine, VnSoundId sound, VnBusId bus,
                     VnEmitterHandle emitter = 0) {
    static const std::vector<float> samples(kSampleRate, 0.05f);
    const VnPCMDesc pcm = MonoDesc(samples, kSampleRate);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    VnPlayDesc play = MakePlayDesc(sound);
    play.busId = bus;
    play.emitterHandle = emitter;
    VnVoiceHandle voice = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    engine->nullBackend.Pump(1);
    return voice;
}

void ClaimVoiceVolumeForRetirement(harken::Engine* engine,
                                   VnVoiceHandle render,
                                   harken::VariableSliceRef owner) {
    (void)engine->params.SetApiDirect(render, harken::kTargetVoiceVolume,
                                      -6.0f);
    ASSERT_TRUE(engine->params.TryClaim(
        render, harken::kTargetVoiceVolume, owner,
        engine->params.TargetRevision(render,
                                      harken::kTargetVoiceVolume)));
}

void ExpectRetiringVoiceOwnerIsolatedAfterReuse(
    harken::Engine* engine, VnVoiceHandle oldRender,
    VnVoiceHandle newRender, harken::VariableSliceRef oldOwner,
    harken::VariableSliceRef newOwner, float expectedNewResolved) {
    ASSERT_EQ(static_cast<std::uint32_t>(oldRender),
              static_cast<std::uint32_t>(newRender));
    ASSERT_NE(static_cast<std::uint32_t>(oldRender >> 32),
              static_cast<std::uint32_t>(newRender >> 32));
    ASSERT_EQ(1u, engine->params.RetiringTargetCountForTests());
    float resolved = 0.0f;
    std::uint32_t rampMs = 0;
    std::uint32_t revision = 0;
    ASSERT_TRUE(engine->params.SnapshotForClaim(
        oldRender, harken::kTargetVoiceVolume, oldOwner, &resolved, &rampMs,
        &revision));
    ASSERT_TRUE(std::isfinite(resolved));
    const float retiredResolved = resolved;
    const float newResolved = engine->params.ResolvedValue(
        newRender, harken::kTargetVoiceVolume);
    EXPECT_FLOAT_EQ(expectedNewResolved, newResolved);
    EXPECT_NE(retiredResolved, newResolved);
    ASSERT_TRUE(engine->params.TryClaim(
        newRender, harken::kTargetVoiceVolume, newOwner,
        engine->params.TargetRevision(newRender,
                                      harken::kTargetVoiceVolume)));
    ASSERT_TRUE(engine->params.SnapshotForClaim(
        newRender, harken::kTargetVoiceVolume, newOwner, &resolved, &rampMs,
        &revision));
    EXPECT_FLOAT_EQ(newResolved, resolved);
    ASSERT_TRUE(engine->params.SnapshotForClaim(
        oldRender, harken::kTargetVoiceVolume, oldOwner, &resolved, &rampMs,
        &revision));
    EXPECT_FLOAT_EQ(retiredResolved, resolved);

    engine->params.ReleaseClaim(oldRender, harken::kTargetVoiceVolume,
                                oldOwner);
    EXPECT_EQ(0u, engine->params.RetiringTargetCountForTests());
    EXPECT_TRUE(engine->params.ForeignClaim(
        newRender, harken::kTargetVoiceVolume));
    ASSERT_TRUE(engine->params.SnapshotForClaim(
        newRender, harken::kTargetVoiceVolume, newOwner, &resolved, &rampMs,
        &revision));
    EXPECT_FLOAT_EQ(newResolved, resolved);
    engine->params.ReleaseClaim(newRender, harken::kTargetVoiceVolume,
                                newOwner);
}

void MakeAcceptedWriteDue(harken::Engine* engine) {
    engine->nullBackend.Pump(8);
    ASSERT_EQ(VN_OK, Vn_Update());
}

harken::VariableSliceTarget BusVolumeSliceTarget(harken::Engine* engine,
                                                float volumeDb) {
    harken::VariableSliceTarget target{};
    target.locator.scope = harken::VariableTargetScope::Bus;
    target.locator.object = 0;
    target.locator.objectGeneration = engine->buses.AudioGenerationAt(0);
    target.locator.targetId = harken::kBusTargetBusVolume;
    target.command.opcode = harken::kOpcodeSetBusVolume;
    target.command.payload.setBusVolume.volumeDb = volumeDb;
    target.command.payload.setBusVolume.busIndex = 0;
    target.command.payload.setBusVolume.rampMs = 0;
    target.command.payload.setBusVolume.busGeneration =
        target.locator.objectGeneration;
    return target;
}

harken::VariableSliceTarget DeadVoiceVolumeSliceTarget(
    harken::Engine* engine, float volumeDb) {
    harken::VariableSliceTarget target{};
    std::uint32_t generation = 0;
    (void)engine->voices.FreeSlotGeneration(0, &generation);
    target.locator.scope = harken::VariableTargetScope::Voice;
    target.locator.object = static_cast<std::uint64_t>(generation) << 32;
    target.locator.objectGeneration = generation;
    target.locator.targetId = harken::kTargetVoiceVolume;
    target.command.opcode = harken::kOpcodeSetVoiceVolume;
    target.command.payload.setVoiceVolume.handle =
        static_cast<VnVoiceHandle>(target.locator.object);
    target.command.payload.setVoiceVolume.volumeDb = volumeDb;
    target.command.payload.setVoiceVolume.rampMs = 0;
    return target;
}

bool FileCurrentSlice(harken::Engine* engine, std::uint64_t executeFrame,
                      std::uint64_t sequence,
                      const harken::VariableSliceTarget* targets,
                      std::uint32_t targetCount,
                      harken::VariableSliceRef* out) {
    if (engine == nullptr || targets == nullptr || targetCount == 0 ||
        out == nullptr) {
        return false;
    }
    *out = {};
    if (!engine->variableSlices.Reserve(1, 1, sequence, executeFrame,
                                        sequence, targetCount, out)) {
        return false;
    }
    for (std::uint32_t i = 0; i < targetCount; ++i) {
        if (!engine->variableSlices.AddTarget(*out, targets[i])) {
            (void)engine->variableSlices.AbandonPrepared(*out);
            *out = {};
            return false;
        }
    }
    harken::ScheduledCurrentSliceInput input{};
    input.executeFrame = executeFrame;
    input.logicCommitSequence = sequence;
    input.slice = {out->slot, out->generation};
    const harken::ReserveResult reserved =
        engine->timelinePool.ReserveCurrentSlice(
            input, harken::PrePublishLatePolicy::Refuse, 0);
    if (reserved.status != harken::ReserveStatus::Committed) {
        (void)engine->variableSlices.AbandonPrepared(*out);
        *out = {};
        return false;
    }
    return engine->variableSlices.Publish(*out);
}

struct CurrentSliceFrontierAdvance {
    harken::Engine* engine = nullptr;
    bool advancedThroughCurrentPage = false;
};

struct CurrentSliceStageObservation {
    harken::Engine* engine = nullptr;
    bool sawIncomplete = false;
    std::uint64_t block = 0;
    std::uint64_t consumerFrontier = 0;
    std::size_t stagedSliceCount = 0;
    std::uint32_t stagedPageCount = 0;
    harken::TimelinePagePrefixStopReason reason =
        harken::TimelinePagePrefixStopReason::Unavailable;
};

struct CurrentSliceScratchReset {
    harken::Engine* engine;
    ~CurrentSliceScratchReset() {
        engine->mixer.RestoreCurrentSliceScratchLimitForTest();
        engine->mixer.SetCurrentSliceFrontierHookForTest(nullptr, nullptr);
    }
};

void AdvanceThroughStagedCurrentSlicePage(void* context,
                                          CurrentSliceHookPhase phase,
                                          std::uint64_t current,
                                          std::size_t stagedCount,
                                          std::uint32_t stagedPageCount,
                                          harken::TimelinePagePrefixStopReason) noexcept {
    CurrentSliceFrontierAdvance& advance =
        *static_cast<CurrentSliceFrontierAdvance*>(context);
    if (advance.advancedThroughCurrentPage || advance.engine == nullptr ||
        phase != CurrentSliceHookPhase::AfterExecuteBeforeApply ||
        stagedPageCount == 0 || stagedCount == 0) {
        return;
    }
    harken::ScheduledTransactionPool& pool = advance.engine->timelinePool;
    while (pool.WindowStartQuantum() <= current &&
           pool.WindowStartQuantum() <
               pool.ConsumerFrontier().load(std::memory_order_acquire)) {
        const std::uint64_t before = pool.WindowStartQuantum();
        (void)pool.Advance();
        if (pool.WindowStartQuantum() == before) {
            break;
        }
    }
    advance.advancedThroughCurrentPage =
        pool.WindowStartQuantum() > current;
}

void ObserveIncompleteCurrentSliceStage(void* context,
                                        CurrentSliceHookPhase phase,
                                        std::uint64_t current,
                                        std::size_t stagedCount,
                                        std::uint32_t stagedPageCount,
                                        harken::TimelinePagePrefixStopReason reason) noexcept {
    CurrentSliceStageObservation& observation =
        *static_cast<CurrentSliceStageObservation*>(context);
    if (phase != CurrentSliceHookPhase::AfterStageBeforeExecute ||
        reason != harken::TimelinePagePrefixStopReason::ScratchCapacity ||
        observation.engine == nullptr) {
        return;
    }
    observation.sawIncomplete = true;
    observation.block = current;
    observation.consumerFrontier =
        observation.engine->timelinePool.ConsumerFrontier().load(
            std::memory_order_acquire);
    observation.stagedSliceCount = stagedCount;
    observation.stagedPageCount = stagedPageCount;
    observation.reason = reason;
}

struct CurrentSlicePrefixObservation {
    bool sawStage = false;
    std::uint64_t block = 0;
    std::size_t stagedSliceCount = 0;
    std::uint32_t stagedPageCount = 0;
    harken::TimelinePagePrefixStopReason reason =
        harken::TimelinePagePrefixStopReason::Unavailable;
};

void ObserveCurrentSlicePrefix(
    void* context, CurrentSliceHookPhase phase,
    std::uint64_t current, std::size_t stagedCount,
    std::uint32_t stagedPageCount,
    harken::TimelinePagePrefixStopReason reason) noexcept {
    if (phase != CurrentSliceHookPhase::AfterStageBeforeExecute) {
        return;
    }
    CurrentSlicePrefixObservation& observation =
        *static_cast<CurrentSlicePrefixObservation*>(context);
    observation.sawStage = true;
    observation.block = current;
    observation.stagedSliceCount = stagedCount;
    observation.stagedPageCount = stagedPageCount;
    observation.reason = reason;
}

struct PublishAfterStageContext {
    harken::Engine* engine = nullptr;
    std::uint64_t publishCutoffFrame = 0;
    bool published = false;
    std::uint32_t stagedPageCount = 0;
    harken::TimelinePagePrefixStopReason reason =
        harken::TimelinePagePrefixStopReason::Unavailable;
};

struct AudioClaimObservation {
    harken::VariableSliceRef ref{};
    std::atomic<std::uint32_t> callCount{0};
};

void ObserveAudioClaim(void* context, harken::VariableSliceRef ref) noexcept {
    AudioClaimObservation& observation =
        *static_cast<AudioClaimObservation*>(context);
    observation.ref = ref;
    observation.callCount.fetch_add(1, std::memory_order_release);
}

struct AudioClaimWaitResult {
    harken::VariableSliceRef ref{};
    std::uint32_t callCount = 0;
};

AudioClaimWaitResult WaitForAudioClaim(harken::Engine* engine) {
    AudioClaimObservation observation{};
    struct HookReset {
        harken::Engine* engine = nullptr;

        ~HookReset() {
            engine->variableSlices.SetAfterAudioClaimHookForTest(nullptr,
                                                                nullptr);
        }
    } hookReset{engine};
    engine->variableSlices.SetAfterAudioClaimHookForTest(
        ObserveAudioClaim, &observation);
    for (std::uint32_t block = 0;
         block < 16 &&
         observation.callCount.load(std::memory_order_acquire) == 0;
         ++block) {
        engine->nullBackend.Pump(1);
    }
    return {observation.ref,
            observation.callCount.load(std::memory_order_acquire)};
}

struct AudioClaimBarrier {
    harken::VariableSliceRef ref{};
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

void BlockAfterAudioClaim(void* context,
                          harken::VariableSliceRef ref) noexcept {
    AudioClaimBarrier& barrier =
        *static_cast<AudioClaimBarrier*>(context);
    barrier.ref = ref;
    barrier.entered.store(true, std::memory_order_release);
    while (!barrier.release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void PublishCurrentPageAfterStage(
    void* context, CurrentSliceHookPhase phase,
    std::uint64_t, std::size_t, std::uint32_t stagedPageCount,
    harken::TimelinePagePrefixStopReason reason) noexcept {
    PublishAfterStageContext& publish =
        *static_cast<PublishAfterStageContext*>(context);
    if (phase != CurrentSliceHookPhase::AfterStageBeforeExecute ||
        publish.published || publish.engine == nullptr) {
        return;
    }
    publish.stagedPageCount = stagedPageCount;
    publish.reason = reason;
    publish.engine->timelinePool.Publish(publish.publishCutoffFrame);
    publish.published = true;
}

TEST(VariableCurrentSlice, VoiceAndBusTargetsApplyInOneAudioSlice) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxBuses = 4;
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(9), VN_BUS_MASTER));
    const VnVoiceHandle voice = PlayDc(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, render, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    RegisterBusVolumeBinding(harkentest::ObjectId(1), harkentest::ObjectId(9));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    engine->mixer.SetMaxCommandsPerQuantumForTest(1);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(engine->timelinePool.WindowStartQuantum(),
              engine->timelinePool.ConsumerFrontier().load(
                  std::memory_order_acquire));
    harken::VoiceRenderContext* voiceState = engine->voices.Resolve(render);
    harken::BusTree::Bus* busState =
        engine->buses.BusState(engine->buses.FindIndex(harkentest::ObjectId(9)));
    ASSERT_NE(nullptr, voiceState);
    ASSERT_NE(nullptr, busState);
    ASSERT_FLOAT_EQ(1.0f, voiceState->gain.target);
    ASSERT_FLOAT_EQ(1.0f, busState->gain.target);

    CurrentSliceFrontierAdvance frontierAdvance{engine};
    engine->mixer.SetCurrentSliceFrontierHookForTest(
        AdvanceThroughStagedCurrentSlicePage, &frontierAdvance);
    for (std::uint32_t block = 0; block < 8 &&
                                  engine->variables.PendingCount() != 0;
         ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    engine->mixer.SetCurrentSliceFrontierHookForTest(nullptr, nullptr);
    EXPECT_TRUE(frontierAdvance.advancedThroughCurrentPage);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f), voiceState->gain.target);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f), busState->gain.target);
}

TEST(VariableCurrentSlice,
     TargetPoolShortageKeepsCandidateInvisibleAndRetriesTheSameOrdinal) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = 2;
    config.maxBindingAttachments = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    RegisterBusVolumeBinding(harkentest::ObjectId(1), VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    std::vector<harken::VariableSliceRef> blockers;
    const std::uint32_t span =
        engine->variableSlices.MaxTargetsPerSlice();
    ASSERT_NE(0u, span);
    for (std::uint64_t ticket = 1;
         engine->variableSlices.FreeTargetCapacity() != 0; ++ticket) {
        const std::uint32_t count = std::min(
            span, engine->variableSlices.FreeTargetCapacity());
        harken::VariableSliceRef ref{};
        ASSERT_TRUE(engine->variableSlices.Reserve(
            0xF00D, 1, ticket, ticket * kBlockFrames, ticket, count, &ref));
        blockers.push_back(ref);
    }
    ASSERT_EQ(0u, engine->variableSlices.FreeTargetCapacity());

    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const float committed = engine->busTargets.LastCommitted(
        0, harken::kBusTargetBusVolume);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    MakeAcceptedWriteDue(engine);

    float current = -1.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(0.0f, current);
    EXPECT_FLOAT_EQ(committed, engine->busTargets.LastCommitted(
                                         0, harken::kBusTargetBusVolume));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(1u, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->variables.AppliedCount());

    for (harken::VariableSliceRef ref : blockers) {
        ASSERT_TRUE(engine->variableSlices.AbandonPrepared(ref));
    }
    for (std::uint32_t block = 0; block < 16 &&
                                  engine->variables.PendingCount() != 0;
         ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    ASSERT_EQ(0u, engine->variables.PendingCount());
    ASSERT_EQ(1u, engine->variables.AppliedCount());
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(1.0f, current);
    harken::VariableOutcomeRecord outcome{};
    ASSERT_TRUE(engine->variables.PopOutcome(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
    EXPECT_EQ(1u, outcome.bankVersionStableOrdinal);
}

TEST(VariableCurrentSlice,
     ExactCensusOverflowAtTwoHundredFiftySevenKeepsDurableWriteWhole) {
    constexpr std::uint32_t kVoiceCount = 32;
    constexpr std::uint32_t kTargetsPerVoice = 8;
    constexpr std::uint32_t kVoiceTargetCount =
        kVoiceCount * kTargetsPerVoice;
    constexpr std::array<float, kTargetsPerVoice> initialValues = {
        0.0f, 0.0f, 20000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    constexpr std::array<float, kTargetsPerVoice> finalValues = {
        -6.0f, 100.0f, 1000.0f, 1000.0f,
        0.5f, 0.75f, 6.0f, 1.4f};

    VnConfig config = MakeNullConfig(kVoiceCount, kBlockFrames, kSampleRate);
    config.commandQueueDepth = 512;
    config.maxControlCommandsPerQuantum = 512;
    config.maxBindings = 2;
    config.maxBindingAttachments = 2;
    config.voiceBindingInstanceSlots = kTargetsPerVoice;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    ASSERT_EQ(kVoiceTargetCount,
              engine->variableSlices.MaxTargetsPerSlice());

    std::array<VnVoiceHandle, kVoiceCount> renders{};
    for (std::uint32_t voiceIndex = 0; voiceIndex < kVoiceCount;
         ++voiceIndex) {
        const VnVoiceHandle voice =
            PlayDc(engine, harkentest::ObjectId(1000 + voiceIndex), VN_BUS_MASTER);
        renders[voiceIndex] = harkentest::RenderHandle(engine, voice);
        ASSERT_NE(0u, renders[voiceIndex]);
        for (std::uint32_t target = 0; target < kTargetsPerVoice;
             ++target) {
            ASSERT_TRUE(engine->params.BindVoiceTarget(
                kVariable, renders[voiceIndex], target,
                LinearCurve(0.0f, initialValues[target],
                            1.0f, finalValues[target])));
        }
    }
    RegisterBusVolumeBinding(harkentest::ObjectId(1), VN_BUS_MASTER, 0.0f, 0.0f,
                             1.0f, -12.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());

    const std::uint32_t freeRows = engine->variableSlices.FreeRowCount();
    const std::uint32_t freeTargets =
        engine->variableSlices.FreeTargetCapacity();
    ASSERT_NE(0u, freeRows);
    ASSERT_GE(freeTargets, kVoiceTargetCount);
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t acceptedBefore = engine->variables.AcceptedCount();
    const std::uint64_t appliedBefore = engine->variables.AppliedCount();
    const float busCommitted = engine->busTargets.LastCommitted(
        0, harken::kBusTargetBusVolume);
    const float firstResolved = engine->params.ResolvedValue(
        renders.front(), harken::kTargetVoiceVolume);
    const float firstCommitted = engine->params.LastCommitted(
        renders.front(), harken::kTargetVoiceVolume);
    const float lastResolved = engine->params.ResolvedValue(
        renders.back(), harken::kTargetVoiceLowpassQ);
    const float lastCommitted = engine->params.LastCommitted(
        renders.back(), harken::kTargetVoiceLowpassQ);
    ASSERT_FLOAT_EQ(firstResolved, firstCommitted);
    ASSERT_FLOAT_EQ(lastResolved, lastCommitted);
    ASSERT_TRUE(std::isfinite(firstCommitted));
    ASSERT_TRUE(std::isfinite(lastCommitted));

    const std::uint64_t issuedBefore = engine->logicCommits.IssuedCount();
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    const std::uint64_t durableTicket = issuedBefore + 1u;
    ASSERT_EQ(durableTicket, engine->logicCommits.IssuedCount());
    MakeAcceptedWriteDue(engine);

    float current = -1.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(0.0f, current);
    EXPECT_EQ(acceptedBefore + 1u, engine->variables.AcceptedCount());
    EXPECT_EQ(appliedBefore, engine->variables.AppliedCount());
    EXPECT_EQ(1u, engine->variables.PendingCount());
    EXPECT_EQ(durableTicket, engine->logicCommits.IssuedCount());
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
    EXPECT_FLOAT_EQ(busCommitted, engine->busTargets.LastCommitted(
                                      0, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(firstResolved, engine->params.ResolvedValue(
                                       renders.front(),
                                       harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(firstCommitted, engine->params.LastCommitted(
                                        renders.front(),
                                        harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(lastResolved, engine->params.ResolvedValue(
                                      renders.back(),
                                      harken::kTargetVoiceLowpassQ));
    EXPECT_FLOAT_EQ(lastCommitted, engine->params.LastCommitted(
                                       renders.back(),
                                       harken::kTargetVoiceLowpassQ));
    for (VnVoiceHandle render : renders) {
        for (std::uint32_t target = 0; target < kTargetsPerVoice;
             ++target) {
            EXPECT_FALSE(engine->params.ForeignClaim(render, target));
        }
    }
    EXPECT_FALSE(engine->busTargets.ForeignClaim(
        0, harken::kBusTargetBusVolume));

    VnBindingAttachDesc detach{};
    detach.structSize = sizeof(detach);
    detach.scopeKind = VN_TARGET_SCOPE_BUS;
    detach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(1), &detach));
    EXPECT_EQ(acceptedBefore + 1u, engine->variables.AcceptedCount());
    EXPECT_EQ(durableTicket, engine->logicCommits.IssuedCount());

    for (std::uint32_t block = 0;
         block < 32 && engine->variables.PendingCount() != 0; ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    ASSERT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(acceptedBefore + 1u, engine->variables.AcceptedCount());
    EXPECT_EQ(appliedBefore + 1u, engine->variables.AppliedCount());
    EXPECT_EQ(durableTicket, engine->logicCommits.IssuedCount());
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(1.0f, current);
    EXPECT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets - kVoiceTargetCount,
              engine->variableSlices.FreeTargetCapacity());
    EXPECT_FLOAT_EQ(finalValues.front(), engine->params.LastCommitted(
                                             renders.front(),
                                             harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(engine->params.ResolvedValue(
                        renders.front(), harken::kTargetVoiceVolume),
                    engine->params.LastCommitted(
                        renders.front(), harken::kTargetVoiceVolume));
    EXPECT_NE(lastCommitted, engine->params.LastCommitted(
                                 renders.back(),
                                 harken::kTargetVoiceLowpassQ));
    EXPECT_FLOAT_EQ(engine->params.ResolvedValue(
                        renders.back(), harken::kTargetVoiceLowpassQ),
                    engine->params.LastCommitted(
                        renders.back(), harken::kTargetVoiceLowpassQ));
    EXPECT_TRUE(std::isfinite(engine->params.LastCommitted(
        renders.back(), harken::kTargetVoiceLowpassQ)));
    for (VnVoiceHandle render : renders) {
        for (std::uint32_t target = 0; target < kTargetsPerVoice;
             ++target) {
            EXPECT_FALSE(engine->params.ForeignClaim(render, target));
        }
    }
    EXPECT_FALSE(engine->busTargets.ForeignClaim(
        0, harken::kBusTargetBusVolume));

    harken::VariableOutcomeRecord outcome{};
    ASSERT_TRUE(engine->variables.PopOutcome(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(1u, outcome.bankVersionStableOrdinal);

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(durableTicket, engine->logicCommits.IssuedCount());
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
}

TEST(VariableCurrentSlice,
     ClaimFailureReleasesOwnedPrefixAndRetriesTheSameDurableWrite) {
    constexpr VnBusId kSecondBus = harkentest::ObjectId(9);
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxBuses = 4;
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSecondBus, VN_BUS_MASTER));
    RegisterBusVolumeBinding(harkentest::ObjectId(1), VN_BUS_MASTER, 0.0f, 0.0f,
                             1.0f, -6.0f);
    RegisterBusVolumeBinding(harkentest::ObjectId(2), kSecondBus, 0.0f, 0.0f,
                             1.0f, -12.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());

    const std::uint32_t firstBus = engine->buses.FindIndex(VN_BUS_MASTER);
    const std::uint32_t secondBus = engine->buses.FindIndex(kSecondBus);
    ASSERT_NE(harken::kInvalidBusIndex, firstBus);
    ASSERT_NE(harken::kInvalidBusIndex, secondBus);
    ASSERT_LT(firstBus, secondBus);
    const float firstCommitted = engine->busTargets.LastCommitted(
        firstBus, harken::kBusTargetBusVolume);
    const float secondCommitted = engine->busTargets.LastCommitted(
        secondBus, harken::kBusTargetBusVolume);
    const std::uint32_t secondRevision = engine->busTargets.TargetRevision(
        secondBus, harken::kBusTargetBusVolume);
    ASSERT_NE(0u, secondRevision);

    harken::VariableSliceRef dummy{};
    ASSERT_TRUE(engine->variableSlices.Reserve(
        engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), 0xD00D,
        engine->mixer.BlocksRendered() * kBlockFrames, 0xD00D, 1,
        &dummy));
    ASSERT_TRUE(engine->busTargets.TryClaim(
        secondBus, harken::kBusTargetBusVolume, dummy, secondRevision));
    const std::uint32_t freeRowsAfterDummy =
        engine->variableSlices.FreeRowCount();
    const std::uint32_t freeTargetsAfterDummy =
        engine->variableSlices.FreeTargetCapacity();
    ASSERT_NE(0u, freeTargetsAfterDummy);
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t acceptedBefore = engine->variables.AcceptedCount();
    const std::uint64_t appliedBefore = engine->variables.AppliedCount();

    const std::uint64_t issuedBefore = engine->logicCommits.IssuedCount();
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    const std::uint64_t durableTicket = issuedBefore + 1u;
    ASSERT_EQ(durableTicket, engine->logicCommits.IssuedCount());
    MakeAcceptedWriteDue(engine);

    float current = -1.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(0.0f, current);
    EXPECT_EQ(acceptedBefore + 1u, engine->variables.AcceptedCount());
    EXPECT_EQ(appliedBefore, engine->variables.AppliedCount());
    EXPECT_EQ(1u, engine->variables.PendingCount());
    EXPECT_EQ(durableTicket, engine->logicCommits.IssuedCount());
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(freeRowsAfterDummy, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargetsAfterDummy,
              engine->variableSlices.FreeTargetCapacity());
    EXPECT_FLOAT_EQ(firstCommitted, engine->busTargets.LastCommitted(
                                        firstBus,
                                        harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(secondCommitted, engine->busTargets.LastCommitted(
                                         secondBus,
                                         harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.ForeignClaim(
        firstBus, harken::kBusTargetBusVolume));
    EXPECT_TRUE(engine->busTargets.ForeignClaim(
        secondBus, harken::kBusTargetBusVolume));
    float dummyResolved = 0.0f;
    std::uint32_t dummyRampMs = UINT32_MAX;
    std::uint32_t dummyRevision = 0;
    ASSERT_TRUE(engine->busTargets.SnapshotForClaim(
        secondBus, harken::kBusTargetBusVolume, dummy, &dummyResolved,
        &dummyRampMs, &dummyRevision));
    EXPECT_FLOAT_EQ(secondCommitted, dummyResolved);
    EXPECT_EQ(0u, dummyRampMs);
    EXPECT_EQ(secondRevision, dummyRevision);

    engine->busTargets.ReleaseClaim(
        secondBus, harken::kBusTargetBusVolume, dummy);
    EXPECT_FALSE(engine->busTargets.ForeignClaim(
        secondBus, harken::kBusTargetBusVolume));
    ASSERT_TRUE(engine->variableSlices.AbandonPrepared(dummy));
    EXPECT_EQ(freeRowsAfterDummy + 1u,
              engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargetsAfterDummy + 1u,
              engine->variableSlices.FreeTargetCapacity());
    EXPECT_EQ(acceptedBefore + 1u, engine->variables.AcceptedCount());
    EXPECT_EQ(durableTicket, engine->logicCommits.IssuedCount());

    for (std::uint32_t block = 0;
         block < 32 && engine->variables.PendingCount() != 0; ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    ASSERT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(acceptedBefore + 1u, engine->variables.AcceptedCount());
    EXPECT_EQ(appliedBefore + 1u, engine->variables.AppliedCount());
    EXPECT_EQ(durableTicket, engine->logicCommits.IssuedCount());
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(1.0f, current);
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(-6.0f, engine->busTargets.LastCommitted(
                               firstBus, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(-12.0f, engine->busTargets.LastCommitted(
                                secondBus, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.ForeignClaim(
        firstBus, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.ForeignClaim(
        secondBus, harken::kBusTargetBusVolume));
    EXPECT_EQ(freeRowsAfterDummy, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargetsAfterDummy - 1u,
              engine->variableSlices.FreeTargetCapacity());

    harken::VariableOutcomeRecord outcome{};
    ASSERT_TRUE(engine->variables.PopOutcome(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(1u, outcome.bankVersionStableOrdinal);

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(durableTicket, engine->logicCommits.IssuedCount());
    EXPECT_EQ(freeRowsAfterDummy + 1u,
              engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargetsAfterDummy + 1u,
              engine->variableSlices.FreeTargetCapacity());
    EXPECT_FALSE(engine->busTargets.ForeignClaim(
        firstBus, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.ForeignClaim(
        secondBus, harken::kBusTargetBusVolume));
}

TEST(VariableCurrentSlice,
     BlockedAttachHasNoMutationAndUnrelatedVariableStillAdmits) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const harken::VariableDefinitionV1 definitions[] = {
        MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth,
                               0.0f, 1.0f, 0.0f),
        MakeVariableDefinition(harkentest::OffsetObjectId(kVariable, 1),
                               harken::VariableKindV1::Smooth,
                               0.0f, 1.0f, 0.0f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, definitions, 2)
                  .status);
    RegisterLinearBinding(harkentest::ObjectId(1), kVariable, VN_TARGET_SCOPE_BUS,
                          harken::kBusTargetBusVolume, 0.0f, -12.0f);
    RegisterLinearBinding(harkentest::ObjectId(2), harkentest::OffsetObjectId(kVariable, 1), VN_TARGET_SCOPE_BUS,
                          harken::kBusTargetBusVolume, 0.0f, -6.0f);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    EXPECT_EQ(VN_OK,
              engine->bindings.CanAttach(harkentest::ObjectId(1), VN_TARGET_SCOPE_BUS,
                                         VN_BUS_MASTER));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = VN_BUS_MASTER;
    EXPECT_EQ(VN_BACKPRESSURE, Vn_AttachBinding(harkentest::ObjectId(1), &attach));
    EXPECT_EQ(VN_OK,
              engine->bindings.CanAttach(harkentest::ObjectId(1), VN_TARGET_SCOPE_BUS,
                                         VN_BUS_MASTER));
    EXPECT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(2), &attach));

    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1), &attach));
}

TEST(VariableCurrentSlice,
     ExactRecensusMismatchReleasesTheClaimedSpanBeforeCandidateProjection) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = 2;
    config.maxBindingAttachments = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    RegisterBusVolumeBinding(harkentest::ObjectId(1), VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    harken::VariableCallObservation observation = harken::ObserveVariableCallAt(
        engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    observation.requestedEffectiveFrame = 0;
    ASSERT_EQ(VN_OK, engine->variables.AcceptGlobal(
                         kVariable, 1.0f, observation, engine->params,
                         engine->bindings, engine->busTargets)
                         .result);
    const std::uint32_t freeRows = engine->variableSlices.FreeRowCount();
    const std::uint32_t freeTargets =
        engine->variableSlices.FreeTargetCapacity();
    engine->variables.BeginService(*engine, engine->mixer.BlocksRendered(),
                                   kBlockFrames);
    ASSERT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    ASSERT_EQ(freeTargets - 1u,
              engine->variableSlices.FreeTargetCapacity());

    VnBindingAttachDesc detach{};
    detach.structSize = sizeof(detach);
    detach.scopeKind = VN_TARGET_SCOPE_BUS;
    detach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(1), &detach));
    engine->variables.PrepareClaimedSlices(*engine);

    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
    EXPECT_EQ(1u, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->variables.AppliedCount());
    float current = -1.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(0.0f, current);
}

TEST(VariableCurrentSlice,
     FrozenVoiceTargetRampsSurviveEveryTransitionCapableOpcodeLane) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = 8;
    config.voiceBindingInstanceSlots = 8;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    const VnVoiceHandle voice = PlayDc(engine, harkentest::ObjectId(20), VN_BUS_MASTER);
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);

    struct BindingCase {
        VnBindingId binding;
        std::uint32_t target;
        float y0;
        float y1;
        std::uint32_t rampMs;
    };
    constexpr std::array<BindingCase, 4> cases = {{
        {harkentest::ObjectId(1), harken::kTargetVoiceVolume, 0.0f, -6.0f, 11},
        {harkentest::ObjectId(2), harken::kTargetVoiceLowpassCutoff, 20000.0f, 1000.0f, 22},
        {harkentest::ObjectId(3), harken::kTargetVoiceLowShelfGainDb, 0.0f, 6.0f, 33},
        {harkentest::ObjectId(4), harken::kTargetVoiceHighShelfGainDb, 0.0f, -6.0f, 44},
    }};
    for (const BindingCase& c : cases) {
        RegisterLinearBinding(c.binding, kVariable, VN_TARGET_SCOPE_VOICE,
                              c.target, c.y0, c.y1, c.rampMs);
        ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(voice, c.binding));
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    for (std::uint32_t block = 0; block < 16 &&
                                  engine->variables.PendingCount() != 0;
         ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    ASSERT_EQ(0u, engine->variables.PendingCount());
    std::array<bool, 4> seen{};
    engine->params.ForEachActiveTarget(
        [&](const harken::ParamSystem::TargetCaptureRow& row) {
            if (row.handle != render) {
                return;
            }
            for (std::size_t i = 0; i < cases.size(); ++i) {
                if (row.targetId == cases[i].target) {
                    EXPECT_EQ(cases[i].rampMs, row.lastPublishedRampMs);
                    seen[i] = true;
                }
            }
        });
    for (bool found : seen) {
        EXPECT_TRUE(found);
    }
}

TEST(VariableCurrentSlice,
     FrozenRampIsNotReplayedByCandidateRestoreOrAppliedBankSync) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = 1;
    config.voiceBindingInstanceSlots = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    const VnVoiceHandle voice = PlayDc(engine, harkentest::ObjectId(21), VN_BUS_MASTER);
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    RegisterLinearBinding(harkentest::ObjectId(1), kVariable, VN_TARGET_SCOPE_VOICE,
                          harken::kTargetVoiceVolume, 0.0f, -3.0f, 73);
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(voice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    const std::uint64_t durableTicket = engine->logicCommits.IssuedCount();
    MakeAcceptedWriteDue(engine);
    ASSERT_TRUE(engine->params.ForeignClaim(
        render, harken::kTargetVoiceVolume));
    const harken::VariableSliceRef slice =
        engine->variables.SliceForTicketForTests(durableTicket);
    ASSERT_TRUE(slice.Valid());
    EXPECT_EQ(harken::VariableSliceState::PageCommitted,
              engine->variableSlices.State(slice));
    const harken::VariableSliceTable& slices = engine->variableSlices;
    ASSERT_EQ(1u, slices.TargetCount(slice));
    const harken::VariableSliceTarget* frozen = slices.Target(slice, 0);
    ASSERT_NE(nullptr, frozen);
    EXPECT_FLOAT_EQ(-3.0f, frozen->resolvedValue);
    EXPECT_EQ(73u, frozen->rampMs);
    EXPECT_EQ(0u, engine->params.PendingRampMs(
                      render, harken::kTargetVoiceVolume));

    engine->params.SetMeterProbeContribution(
        render, harkentest::ObjectId(0xC011u), harken::kTargetVoiceVolume, -1.0f, 17);
    EXPECT_FLOAT_EQ(-1.0f, engine->params.ResolvedValue(
                               render, harken::kTargetVoiceVolume));
    EXPECT_EQ(17u, engine->params.PendingRampMs(
                       render, harken::kTargetVoiceVolume));
    const AudioClaimWaitResult observation = WaitForAudioClaim(engine);
    ASSERT_EQ(1u, observation.callCount);
    ASSERT_EQ(harken::VariableSliceState::SliceApplied,
              engine->variableSlices.State(observation.ref));

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_FALSE(engine->params.ForeignClaim(
        render, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(-3.0f, engine->params.LastCommitted(
                               render, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(-4.0f, engine->params.ResolvedValue(
                               render, harken::kTargetVoiceVolume));
    EXPECT_EQ(17u, engine->params.PendingRampMs(
                       render, harken::kTargetVoiceVolume));
    EXPECT_TRUE(engine->params.PendingPublication(
        render, harken::kTargetVoiceVolume));

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(-4.0f, engine->params.LastCommitted(
                               render, harken::kTargetVoiceVolume));
    bool foundTarget = false;
    engine->params.ForEachActiveTarget(
        [&](const harken::ParamSystem::TargetCaptureRow& target) {
            if (target.handle == render &&
                target.targetId == harken::kTargetVoiceVolume) {
                EXPECT_EQ(17u, target.lastPublishedRampMs);
                foundTarget = true;
            }
        });
    EXPECT_TRUE(foundTarget);
    EXPECT_FALSE(engine->params.PendingPublication(
        render, harken::kTargetVoiceVolume));
}

TEST(VariableCurrentSlice,
     PendingBindingRevisionWaitsForItsVariableBankToBecomeTerminal) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = 2;
    config.maxBindingAttachments = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    RegisterBusVolumeBinding(harkentest::ObjectId(1), VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Vn_Update());

    const harken::BindingTable::Definition* before =
        engine->bindings.Find(harkentest::ObjectId(1));
    ASSERT_NE(nullptr, before);
    const std::uint32_t revision = before->revision;
    VnBindingDesc replacement{};
    replacement.structSize = sizeof(replacement);
    replacement.variableId = kVariable;
    replacement.targetScopeKind = VN_TARGET_SCOPE_BUS;
    replacement.targetId = harken::kBusTargetBusVolume;
    replacement.transitionMs = 17;
    replacement.curve.structSize = sizeof(replacement.curve);
    replacement.curve.pointCount = 2;
    replacement.curve.points[0] = {0.0f, 0.0f};
    replacement.curve.points[1] = {1.0f, -18.0f};
    replacement.curve.handles[0] = harkentest::EndpointHandles(
        replacement.curve.points[0], replacement.curve.points[1]);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    ASSERT_EQ(VN_OK, Vn_ReplaceBinding(harkentest::ObjectId(1), &replacement));
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::BindingTable::Definition* blocked =
        engine->bindings.Find(harkentest::ObjectId(1));
    ASSERT_NE(nullptr, blocked);
    EXPECT_TRUE(blocked->pendingInstall);
    EXPECT_EQ(revision, blocked->revision);

    harkentest::AdvanceVariableWrite(engine);
    const harken::BindingTable::Definition* installed =
        engine->bindings.Find(harkentest::ObjectId(1));
    ASSERT_NE(nullptr, installed);
    EXPECT_FALSE(installed->pendingInstall);
    EXPECT_EQ(revision + 1u, installed->revision);
    EXPECT_EQ(17u, installed->transitionMs);
}

TEST(VariableCurrentSlice,
     ScratchShortfallAdvancesOnlyTheCompletePrefixAndKeepsTargetPagePending) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = 2;
    config.maxBindingAttachments = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    RegisterBusVolumeBinding(harkentest::ObjectId(1), VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    const std::uint32_t freeTargetsBefore =
        engine->variableSlices.FreeTargetCapacity();
    const std::uint32_t freeRowsBefore =
        engine->variableSlices.FreeRowCount();
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    MakeAcceptedWriteDue(engine);
    CurrentSliceStageObservation observation{engine};
    CurrentSliceScratchReset scratchReset{engine};
    engine->mixer.SetCurrentSliceScratchLimitForTest(0);
    engine->mixer.SetCurrentSliceFrontierHookForTest(
        ObserveIncompleteCurrentSliceStage, &observation);
    std::uint64_t beforeFrontier = 0;
    std::uint64_t emptyPrefixEnd = 0;
    for (std::uint32_t block = 0; block < 8 && !observation.sawIncomplete;
         ++block) {
        beforeFrontier = engine->timelinePool.ConsumerFrontier().load();
        emptyPrefixEnd = engine->timelinePool.PublishedEmptyPrefixEnd(
            beforeFrontier, engine->mixer.BlocksRendered());
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    engine->mixer.SetCurrentSliceFrontierHookForTest(nullptr, nullptr);
    engine->mixer.RestoreCurrentSliceScratchLimitForTest();

    ASSERT_TRUE(observation.sawIncomplete);
    ASSERT_EQ(observation.stagedPageCount, 0u);
    EXPECT_GE(emptyPrefixEnd, beforeFrontier);
    EXPECT_EQ(emptyPrefixEnd, observation.consumerFrontier);
    EXPECT_EQ(observation.stagedSliceCount, 0u);
    EXPECT_EQ(observation.reason,
              harken::TimelinePagePrefixStopReason::ScratchCapacity);
    const std::uint64_t targetPage =
        observation.consumerFrontier + observation.stagedPageCount;
    EXPECT_LE(targetPage, observation.block);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(), targetPage);
    EXPECT_EQ(1u, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->variables.AppliedCount());
    EXPECT_EQ(engine->variableSlices.FreeTargetCapacity() + 1u,
              freeTargetsBefore);
    EXPECT_EQ(engine->variableSlices.FreeRowCount() + 1u, freeRowsBefore);
    harken::BusTree::Bus* busState = engine->buses.BusState(0);
    ASSERT_NE(nullptr, busState);
    EXPECT_FLOAT_EQ(1.0f, busState->gain.target);

    for (std::uint32_t block = 0; block < 8 &&
                                  engine->variables.PendingCount() != 0;
         ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(1u, engine->variables.AppliedCount());
    EXPECT_GT(engine->timelinePool.ConsumerFrontier().load(), targetPage);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f), busState->gain.target);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(engine->variableSlices.FreeTargetCapacity(), freeTargetsBefore);
    EXPECT_EQ(engine->variableSlices.FreeRowCount(), freeRowsBefore);
}

TEST(VariableCurrentSlice, ArenaShortfallDefersThenAppliesTheOverdueSlice) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = 2;
    config.maxBindingAttachments = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    RegisterBusVolumeBinding(harkentest::ObjectId(1), VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    MakeAcceptedWriteDue(engine);
    harken::BusTree::Bus* busState = engine->buses.BusState(0);
    ASSERT_NE(nullptr, busState);
    ASSERT_FLOAT_EQ(1.0f, busState->gain.target);
    engine->mixer.ArenaForTests().SetCapacityForTests(0);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->variables.AppliedCount());
    EXPECT_FLOAT_EQ(1.0f, busState->gain.target);
    engine->mixer.ArenaForTests().RestoreCapacityForTests();

    for (std::uint32_t block = 0; block < 8 &&
                                  engine->variables.PendingCount() != 0;
         ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }

    EXPECT_EQ(1u, engine->variables.AppliedCount());
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f), busState->gain.target);
}

TEST(VariableCurrentSlice, PublishAfterStageWaitsForTheRecoveryBlock) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1);
    const std::uint64_t firstQuantum =
        engine->timelinePool.ConsumerFrontier().load(
            std::memory_order_acquire);
    const std::uint64_t firstFrame =
        engine->timelinePool.PublishCutoffFrame();
    ASSERT_EQ(firstFrame, firstQuantum * kBlockFrames);
    harken::BusTree::Bus* busState = engine->buses.BusState(0);
    ASSERT_NE(nullptr, busState);
    ASSERT_FLOAT_EQ(1.0f, busState->gain.target);
    const harken::VariableSliceTarget target =
        BusVolumeSliceTarget(engine, -12.0f);
    harken::VariableSliceRef ref{};
    ASSERT_TRUE(FileCurrentSlice(engine, firstFrame, 1, &target, 1, &ref));
    ASSERT_EQ(engine->timelinePool.PageStamp(firstQuantum),
              harken::kUnpublishedQuantumStamp);

    PublishAfterStageContext publish{
        engine, firstFrame + kBlockFrames};
    const std::uint64_t deadlineMissesBefore =
        engine->timelineExecutor.Stats().deadlineMissQuanta;
    engine->mixer.SetCurrentSliceFrontierHookForTest(
        PublishCurrentPageAfterStage, &publish);
    engine->nullBackend.Pump(1);
    engine->mixer.SetCurrentSliceFrontierHookForTest(nullptr, nullptr);

    ASSERT_TRUE(publish.published);
    EXPECT_EQ(publish.stagedPageCount, 0u);
    EXPECT_EQ(publish.reason,
              harken::TimelinePagePrefixStopReason::Unpublished);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(), firstQuantum);
    EXPECT_EQ(engine->timelineExecutor.Stats().deadlineMissQuanta,
              deadlineMissesBefore + 1);
    EXPECT_EQ(engine->variableSlices.State(ref),
              harken::VariableSliceState::PageCommitted);
    EXPECT_FLOAT_EQ(1.0f, busState->gain.target);

    engine->nullBackend.Pump(1);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(),
              firstQuantum + 1);
    EXPECT_EQ(engine->variableSlices.State(ref),
              harken::VariableSliceState::SliceApplied);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f), busState->gain.target);
    const std::uint64_t appliedFence =
        engine->variableSlices.AppliedBlockSequence(ref);
    engine->nullBackend.Pump(1);
    EXPECT_EQ(engine->variableSlices.AppliedBlockSequence(ref), appliedFence);
}

TEST(VariableCurrentSlice, PublishedEmptyPageAdvancesTheFixedPrefix) {
    EngineSession session(MakeNullConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->timelinePool.Publish(kBlockFrames);
    CurrentSlicePrefixObservation observation{};
    engine->mixer.SetCurrentSliceFrontierHookForTest(
        ObserveCurrentSlicePrefix, &observation);

    engine->nullBackend.Pump(1);
    engine->mixer.SetCurrentSliceFrontierHookForTest(nullptr, nullptr);

    ASSERT_TRUE(observation.sawStage);
    EXPECT_EQ(observation.stagedSliceCount, 0u);
    EXPECT_EQ(observation.stagedPageCount, 1u);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(), 1u);
}

TEST(VariableCurrentSlice, TwoPagePrefixAppliesSlicesInPageOrder) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1);
    const std::uint64_t firstQuantum =
        engine->timelinePool.ConsumerFrontier().load(
            std::memory_order_acquire);
    const std::uint64_t firstFrame =
        engine->timelinePool.PublishCutoffFrame();
    ASSERT_EQ(firstFrame, firstQuantum * kBlockFrames);
    const harken::VariableSliceTarget firstTarget =
        BusVolumeSliceTarget(engine, -6.0f);
    const harken::VariableSliceTarget secondTarget =
        BusVolumeSliceTarget(engine, -12.0f);
    harken::VariableSliceRef first{};
    harken::VariableSliceRef second{};
    ASSERT_TRUE(FileCurrentSlice(engine, firstFrame, 1, &firstTarget, 1,
                                 &first));
    ASSERT_TRUE(FileCurrentSlice(engine, firstFrame + kBlockFrames, 2,
                                 &secondTarget, 1, &second));
    engine->timelinePool.Publish(firstFrame + 2 * kBlockFrames);

    engine->mixer.ArenaForTests().SetCapacityForTests(0);
    engine->nullBackend.Pump(1);
    engine->mixer.ArenaForTests().RestoreCapacityForTests();
    ASSERT_EQ(engine->timelinePool.ConsumerFrontier().load(), firstQuantum);

    CurrentSlicePrefixObservation observation{};
    engine->mixer.SetCurrentSliceFrontierHookForTest(
        ObserveCurrentSlicePrefix, &observation);
    engine->nullBackend.Pump(1);
    engine->mixer.SetCurrentSliceFrontierHookForTest(nullptr, nullptr);

    ASSERT_TRUE(observation.sawStage);
    EXPECT_EQ(observation.stagedSliceCount, 2u);
    EXPECT_EQ(observation.stagedPageCount, 2u);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(),
              firstQuantum + 2);
    EXPECT_EQ(engine->variableSlices.State(first),
              harken::VariableSliceState::SliceApplied);
    EXPECT_EQ(engine->variableSlices.State(second),
              harken::VariableSliceState::SliceApplied);
    harken::BusTree::Bus* busState = engine->buses.BusState(0);
    ASSERT_NE(nullptr, busState);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f), busState->gain.target);
}

TEST(VariableCurrentSlice, EffectiveCapacityCoversTwoFullLegalPages) {
    const auto publicConfig = MakeNullConfig(2, kBlockFrames, kSampleRate);
    for (const bool shortCapacity : {false, true}) {
        harken::EngineConfig resolved{};
        ASSERT_EQ(VN_OK, harken::ResolveConfig(&publicConfig, &resolved));
        resolved.maxEventsPerQuantum = 4;
        resolved.maxFrameBatchesPerQuantum = 4;
        auto engine = std::make_unique<harken::Engine>(resolved);
        std::vector<float> output(kBlockFrames * 2u);
        const auto render = [&] { engine->mixer.RenderBlock(output.data(), kBlockFrames, 2); };
        for (std::uint32_t block = 0; block < engine->timelinePool.PublishLeadQuanta() + 2u &&
             engine->timelinePool.ConsumerFrontier().load() >= engine->mixer.BlocksRendered(); ++block) {
            render();
        }
        const auto frontier = engine->timelinePool.ConsumerFrontier().load();
        ASSERT_LT(frontier, engine->mixer.BlocksRendered());
        const auto maximum = engine->timelinePool.MaxCurrentSlicesPerQuantum();
        ASSERT_EQ(4u, maximum);
        std::vector<harken::VariableSliceRef> refs(2u * maximum);
        const auto first = BusVolumeSliceTarget(engine.get(), -6.0f);
        const auto second = BusVolumeSliceTarget(engine.get(), -12.0f);
        for (std::uint32_t index = 0; index < refs.size(); ++index) {
            ASSERT_TRUE(FileCurrentSlice(engine.get(),
                (frontier + index / maximum) * kBlockFrames, index + 1u,
                index < maximum ? &first : &second, 1, &refs[index]));
        }
        engine->timelinePool.Publish((frontier + 2u) * kBlockFrames);
        const auto allocated = engine->mixer.CurrentSliceScratchCapacityForTest();
        engine->mixer.SetCurrentSliceScratchLimitForTest(2u * maximum - shortCapacity);
        EXPECT_EQ(allocated, engine->mixer.CurrentSliceScratchCapacityForTest());
        EXPECT_EQ(2u * maximum - shortCapacity, engine->mixer.EffectiveCurrentSliceCapacity());
        CurrentSlicePrefixObservation observed{};
        engine->mixer.SetCurrentSliceFrontierHookForTest(ObserveCurrentSlicePrefix, &observed);
        render();
        const auto consumedPages = shortCapacity ? 1u : 2u;
        ASSERT_TRUE(observed.sawStage);
        EXPECT_EQ(consumedPages, observed.stagedPageCount);
        EXPECT_EQ(consumedPages * maximum, observed.stagedSliceCount);
        EXPECT_EQ(frontier + consumedPages, engine->timelinePool.ConsumerFrontier().load());
        for (std::uint32_t index = 0; index < refs.size(); ++index) {
            EXPECT_EQ(index < consumedPages * maximum ? harken::VariableSliceState::SliceApplied
                                                     : harken::VariableSliceState::PageCommitted,
                engine->variableSlices.State(refs[index]));
        }
        if (shortCapacity) {
            EXPECT_EQ(harken::TimelinePagePrefixStopReason::ScratchCapacity, observed.reason);
            engine->mixer.RestoreCurrentSliceScratchLimitForTest();
            render();
            EXPECT_EQ(frontier + 2u, engine->timelinePool.ConsumerFrontier().load());
        }
        engine->mixer.SetCurrentSliceFrontierHookForTest(nullptr, nullptr);
    }
}

TEST(VariableCurrentSlice, ScratchShortfallConsumesOnlyTheCompleteFirstPage) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1);
    const std::uint64_t firstQuantum =
        engine->timelinePool.ConsumerFrontier().load(
            std::memory_order_acquire);
    const std::uint64_t firstFrame =
        engine->timelinePool.PublishCutoffFrame();
    ASSERT_EQ(firstFrame, firstQuantum * kBlockFrames);
    const harken::VariableSliceTarget firstTarget =
        BusVolumeSliceTarget(engine, -6.0f);
    const harken::VariableSliceTarget secondTarget =
        BusVolumeSliceTarget(engine, -12.0f);
    harken::VariableSliceRef first{};
    harken::VariableSliceRef second{};
    ASSERT_TRUE(FileCurrentSlice(engine, firstFrame, 1, &firstTarget, 1,
                                 &first));
    ASSERT_TRUE(FileCurrentSlice(engine, firstFrame + kBlockFrames, 2,
                                 &secondTarget, 1, &second));
    engine->timelinePool.Publish(firstFrame + 2 * kBlockFrames);

    engine->mixer.ArenaForTests().SetCapacityForTests(0);
    engine->nullBackend.Pump(1);
    engine->mixer.ArenaForTests().RestoreCapacityForTests();
    engine->mixer.SetCurrentSliceScratchLimitForTest(1);
    CurrentSlicePrefixObservation observation{};
    engine->mixer.SetCurrentSliceFrontierHookForTest(
        ObserveCurrentSlicePrefix, &observation);
    engine->nullBackend.Pump(1);
    engine->mixer.SetCurrentSliceFrontierHookForTest(nullptr, nullptr);

    ASSERT_TRUE(observation.sawStage);
    EXPECT_EQ(observation.stagedSliceCount, 1u);
    EXPECT_EQ(observation.stagedPageCount, 1u);
    EXPECT_EQ(observation.reason,
              harken::TimelinePagePrefixStopReason::ScratchCapacity);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(),
              firstQuantum + 1);
    EXPECT_EQ(engine->variableSlices.State(first),
              harken::VariableSliceState::SliceApplied);
    EXPECT_EQ(engine->variableSlices.State(second),
              harken::VariableSliceState::PageCommitted);
    harken::BusTree::Bus* busState = engine->buses.BusState(0);
    ASSERT_NE(nullptr, busState);
    EXPECT_FLOAT_EQ(DbToLinear(-6.0f), busState->gain.target);

    engine->mixer.RestoreCurrentSliceScratchLimitForTest();
    engine->nullBackend.Pump(1);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(),
              firstQuantum + 2);
    EXPECT_EQ(engine->variableSlices.State(second),
              harken::VariableSliceState::SliceApplied);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f), busState->gain.target);
}

TEST(VariableCurrentSlice, GlobalSliceDeadVoiceStillAppliesLiveBusMember) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1);
    const std::uint64_t firstQuantum =
        engine->timelinePool.ConsumerFrontier().load(
            std::memory_order_acquire);
    const std::uint64_t firstFrame =
        engine->timelinePool.PublishCutoffFrame();
    ASSERT_EQ(firstFrame, firstQuantum * kBlockFrames);
    const std::array<harken::VariableSliceTarget, 2> targets = {
        DeadVoiceVolumeSliceTarget(engine, -30.0f),
        BusVolumeSliceTarget(engine, -9.0f)};
    ASSERT_NE(targets[0].locator.objectGeneration, 0u);
    ASSERT_EQ(engine->voices.Resolve(
                  static_cast<VnVoiceHandle>(targets[0].locator.object)),
              nullptr);
    harken::VariableSliceRef ref{};
    ASSERT_TRUE(FileCurrentSlice(
        engine, firstFrame, 1, targets.data(),
        static_cast<std::uint32_t>(targets.size()), &ref));
    engine->timelinePool.Publish(firstFrame + kBlockFrames);

    engine->nullBackend.Pump(1);

    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(),
              firstQuantum + 1);
    EXPECT_EQ(engine->variableSlices.State(ref),
              harken::VariableSliceState::SliceApplied);
    harken::BusTree::Bus* busState = engine->buses.BusState(0);
    ASSERT_NE(nullptr, busState);
    EXPECT_FLOAT_EQ(DbToLinear(-9.0f), busState->gain.target);
}

TEST(VariableCurrentSlice, EffectAndBusWaitForTheSameSlicePage) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.commandQueueDepth = 4;
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);

    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.gainDb = 0.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_GAIN,
                                     reinterpret_cast<const VnEffectDesc*>(&gain)));
    engine->nullBackend.Pump(1);

    VnBindingDesc effect{};
    effect.structSize = sizeof(effect);
    effect.variableId = kVariable;
    effect.targetScopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    effect.targetId = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    effect.curve.structSize = sizeof(effect.curve);
    effect.curve.pointCount = 2;
    effect.curve.points[0] = {0.0f, 0.0f};
    effect.curve.points[1] = {1.0f, -6.0f};
    effect.curve.handles[0] = harkentest::EndpointHandles(
        effect.curve.points[0], effect.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &effect));
    VnBindingAttachDesc effectAttach{};
    effectAttach.structSize = sizeof(effectAttach);
    effectAttach.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    effectAttach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1), &effectAttach));
    RegisterBusVolumeBinding(harkentest::ObjectId(2), VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    engine->nullBackend.Pump(8);
    for (std::uint32_t i = 0; i < 3; ++i) {
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0));
    }
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_FLOAT_EQ(0.0f,
                    engine->effectParamTargets.LastCommitted(
                        harken::EffectParamTargetStore::Scope::Bus, 0, 0,
                        harken::kGainParamGainDb));
    EXPECT_FLOAT_EQ(0.0f, engine->busTargets.LastCommitted(
                              0, harken::kBusTargetBusVolume));
}

TEST(VariableCurrentSlice,
     StaleEffectAttachmentCannotCreatePhantomSliceTargets) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = 2;
    config.maxBindingAttachments = 2;
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 2;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);

    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.gainDb = 0.0f;
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(
                  VN_BUS_MASTER, 0, VN_EFFECT_GAIN,
                  reinterpret_cast<const VnEffectDesc*>(&gain)));
    engine->nullBackend.Pump(1);

    VnBindingDesc effect{};
    effect.structSize = sizeof(effect);
    effect.variableId = kVariable;
    effect.targetScopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    effect.targetId = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    effect.curve.structSize = sizeof(effect.curve);
    effect.curve.pointCount = 2;
    effect.curve.points[0] = {0.0f, 0.0f};
    effect.curve.points[1] = {1.0f, -6.0f};
    effect.curve.handles[0] = harkentest::EndpointHandles(
        effect.curve.points[0], effect.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &effect));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1), &attach));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    const std::uint32_t busIndex = engine->buses.FindIndex(VN_BUS_MASTER);
    ASSERT_NE(harken::kInvalidBusIndex, busIndex);
    const std::uint32_t busGeneration =
        engine->buses.GenerationAt(busIndex);
    std::uint32_t revision = 0;
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Bus, busIndex, busGeneration, 0,
        harken::kGainParamGainDb, VN_EFFECT_GAIN, &revision));
    ASSERT_NE(0u, revision);
    ASSERT_EQ(VN_OK, Vn_ClearBusEffect(VN_BUS_MASTER, 0));
    engine->nullBackend.Pump(1);
    ASSERT_FALSE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Bus, busIndex, busGeneration, 0,
        harken::kGainParamGainDb, VN_EFFECT_GAIN, &revision));

    const auto applyWithoutPhysicalTarget = [&](float value) {
        const std::uint32_t freeRows =
            engine->variableSlices.FreeRowCount();
        const std::uint32_t freeTargets =
            engine->variableSlices.FreeTargetCapacity();
        harken::VariableCallObservation observation =
            harken::ObserveVariableCallAt(
                engine->variables.CurrentWorldGeneration(),
                engine->variables.CurrentTransportEpoch(), kBlockFrames,
                engine->mixer.BlocksRendered() * kBlockFrames,
                engine->mixer.BlocksRendered() * kBlockFrames);
        observation.requestedEffectiveFrame =
            engine->mixer.BlocksRendered() * kBlockFrames;
        ASSERT_EQ(VN_OK,
                  engine->variables.AcceptGlobal(
                      kVariable, value, observation, engine->params,
                      engine->bindings, engine->busTargets)
                      .result);
        engine->variables.BeginService(*engine,
                                       engine->mixer.BlocksRendered(),
                                       kBlockFrames);
        engine->variables.EndService(*engine);
        EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
        EXPECT_EQ(freeTargets,
                  engine->variableSlices.FreeTargetCapacity());
        EXPECT_EQ(1u, engine->variables.PendingCount());
        engine->nullBackend.Pump(1);
        engine->variables.BeginService(*engine,
                                       engine->mixer.BlocksRendered(),
                                       kBlockFrames);
        engine->variables.EndService(*engine);
        EXPECT_EQ(0u, engine->variables.PendingCount());
        float current = 0.0f;
        ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
        EXPECT_FLOAT_EQ(value, current);
        EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
        EXPECT_EQ(freeTargets,
                  engine->variableSlices.FreeTargetCapacity());
    };

    applyWithoutPhysicalTarget(0.25f);

    VnCompressorDesc compressor{};
    compressor.structSize = sizeof(compressor);
    compressor.thresholdDb = -18.0f;
    compressor.ratio = 4.0f;
    compressor.attackMs = 10.0f;
    compressor.releaseMs = 100.0f;
    compressor.kneeDb = 6.0f;
    compressor.makeupDb = 0.0f;
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(
                  VN_BUS_MASTER, 0, VN_EFFECT_COMPRESSOR,
                  reinterpret_cast<const VnEffectDesc*>(&compressor)));
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Bus, busIndex, busGeneration, 0,
        harken::kCompressorParamThresholdDb, VN_EFFECT_COMPRESSOR,
        &revision));
    EXPECT_FALSE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Bus, busIndex, busGeneration, 0,
        harken::kGainParamGainDb, VN_EFFECT_GAIN, &revision));
    applyWithoutPhysicalTarget(0.5f);

    ASSERT_EQ(VN_OK, Vn_ClearBusEffect(VN_BUS_MASTER, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(
                  VN_BUS_MASTER, 0, VN_EFFECT_GAIN,
                  reinterpret_cast<const VnEffectDesc*>(&gain)));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(-3.0f,
                    engine->effectParamTargets.LastCommitted(
                        harken::EffectParamTargetStore::Scope::Bus, busIndex, 0,
                        harken::kGainParamGainDb));
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Bus, busIndex, busGeneration, 0,
        harken::kGainParamGainDb, VN_EFFECT_GAIN, &revision));

    const std::uint32_t freeRows = engine->variableSlices.FreeRowCount();
    const std::uint32_t freeTargets =
        engine->variableSlices.FreeTargetCapacity();
    harken::VariableCallObservation physical = harken::ObserveVariableCallAt(
        engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), kBlockFrames,
        engine->mixer.BlocksRendered() * kBlockFrames,
        engine->mixer.BlocksRendered() * kBlockFrames);
    physical.requestedEffectiveFrame =
        engine->mixer.BlocksRendered() * kBlockFrames;
    ASSERT_EQ(VN_OK,
              engine->variables.AcceptGlobal(
                  kVariable, 1.0f, physical, engine->params,
                  engine->bindings, engine->busTargets)
                  .result);
    engine->variables.BeginService(*engine, engine->mixer.BlocksRendered(),
                                   kBlockFrames);
    EXPECT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets - 1u,
              engine->variableSlices.FreeTargetCapacity());
    EXPECT_TRUE(engine->effectParamTargets.ForeignClaim(
        harken::EffectParamTargetStore::Scope::Bus, busIndex, busGeneration, 0,
        harken::kGainParamGainDb));

    harkentest::AdvanceVariableWrite(engine);
    EXPECT_FLOAT_EQ(-6.0f,
                    engine->effectParamTargets.LastCommitted(
                        harken::EffectParamTargetStore::Scope::Bus, busIndex, 0,
                        harken::kGainParamGainDb));
    EXPECT_FALSE(engine->effectParamTargets.PendingPublication(
        harken::EffectParamTargetStore::Scope::Bus, busIndex, 0,
        harken::kGainParamGainDb));
    EXPECT_FALSE(engine->effectParamTargets.ForeignClaim(
        harken::EffectParamTargetStore::Scope::Bus, busIndex, busGeneration, 0,
        harken::kGainParamGainDb));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
}

TEST(VariableCurrentSlice, NoConsumerCompletesOnlyAfterFollowingBlockTail) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);

    harken::VariableCallObservation observation = harken::ObserveVariableCallAt(
        engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    observation.requestedEffectiveFrame = 4 * kBlockFrames;
    ASSERT_EQ(VN_OK, engine->variables.AcceptGlobal(
                         kVariable, 1.0f, observation, engine->params,
                         engine->bindings, engine->busTargets)
                         .result);
    engine->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->variables.AppliedCount());

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(1u, engine->variables.AppliedCount());
}

TEST(VariableCurrentSlice, LaterUnrelatedCommandsDoNotRaiseCapturedFrontier) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxBuses = 4;
    config.maxBindings = 2;
    config.maxBindingAttachments = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(9), VN_BUS_MASTER));
    RegisterBusVolumeBinding(harkentest::ObjectId(1), harkentest::ObjectId(9));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    engine->mixer.SetMaxCommandsPerQuantumForTest(1);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    MakeAcceptedWriteDue(engine);
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0));
    for (std::uint32_t block = 0; block < 8 &&
                                  engine->variables.PendingCount() != 0;
         ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0));
        ASSERT_EQ(VN_OK, Vn_Update());
    }

    EXPECT_EQ(1u, engine->variables.AppliedCount());
    EXPECT_EQ(0u, engine->variables.PendingCount());
}

TEST(VariableCurrentSlice, EmitterLifecycleResolvesAroundAudioClaim) {
    {
        VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
        config.maxEmitters = 2;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        InstallVariable(engine);
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        const VnVoiceHandle first = PlayDc(engine, harkentest::ObjectId(1), VN_BUS_MASTER, emitter);
        const VnVoiceHandle second = PlayDc(engine, harkentest::ObjectId(2), VN_BUS_MASTER, emitter);
        const VnVoiceHandle firstRender = harkentest::RenderHandle(engine, first);
        const VnVoiceHandle secondRender = harkentest::RenderHandle(engine, second);
        ASSERT_TRUE(engine->params.BindVoiceTarget(
            kVariable, firstRender, harken::kTargetVoiceVolume,
            LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
        ASSERT_TRUE(engine->params.BindVoiceTarget(
            kVariable, secondRender, harken::kTargetVoiceVolume,
            LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(2);

        ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, 1.0f));
        MakeAcceptedWriteDue(engine);
        ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
        engine->nullBackend.Pump(1);
        ASSERT_NE(nullptr, engine->voices.Resolve(firstRender));
        ASSERT_NE(nullptr, engine->voices.Resolve(secondRender));
        EXPECT_FLOAT_EQ(1.0f,
                        engine->voices.Resolve(firstRender)->gain.target);
        EXPECT_FLOAT_EQ(1.0f,
                        engine->voices.Resolve(secondRender)->gain.target);
    }
    {
        VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
        config.maxEmitters = 2;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        InstallVariable(engine);
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        const VnVoiceHandle voice = PlayDc(engine, harkentest::ObjectId(3), VN_BUS_MASTER, emitter);
        const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
        ASSERT_TRUE(engine->params.BindVoiceTarget(
            kVariable, render, harken::kTargetVoiceVolume,
            LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(2);

        ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, 1.0f));
        MakeAcceptedWriteDue(engine);
        for (std::uint32_t block = 0; block < 8 &&
                                      engine->variables.PendingCount() != 0;
             ++block) {
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(1u, engine->variables.AppliedCount());
        EXPECT_EQ(0u, engine->variables.TerminationCount());
    }
}

TEST(VariableCurrentSlice,
     PendingPublicationRemainsScopedAcrossLogicOnlyUpdates) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 3;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    VnEmitterHandle firstEmitter = 0;
    VnEmitterHandle secondEmitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&firstEmitter));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&secondEmitter));
    const VnVoiceHandle first =
        PlayDc(engine, harkentest::ObjectId(38), VN_BUS_MASTER, firstEmitter);
    const VnVoiceHandle second =
        PlayDc(engine, harkentest::ObjectId(39), VN_BUS_MASTER, secondEmitter);
    const VnVoiceHandle firstRender = harkentest::RenderHandle(engine, first);
    const VnVoiceHandle secondRender =
        harkentest::RenderHandle(engine, second);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, firstRender, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, secondRender, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    const auto pending = [&](VnVoiceHandle render) {
        return engine->params.PendingPublication(
                   render, harken::kTargetVoiceVolume) ||
               engine->variables.HasPendingWriteForVoiceTarget(
                   engine->params, render, harken::kTargetVoiceVolume);
    };
    EXPECT_FALSE(pending(firstRender));
    EXPECT_FALSE(pending(secondRender));

    ASSERT_EQ(VN_OK,
              Vn_SetEmitterValue(firstEmitter, kVariable, 1.0f));
    EXPECT_TRUE(pending(firstRender));
    EXPECT_FALSE(pending(secondRender));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(pending(firstRender));
    EXPECT_FALSE(pending(secondRender));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_FALSE(pending(firstRender));
    EXPECT_FALSE(pending(secondRender));
    EXPECT_FLOAT_EQ(-12.0f, engine->params.ResolvedValue(
                                  firstRender,
                                  harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(0.0f, engine->params.ResolvedValue(
                                 secondRender,
                                 harken::kTargetVoiceVolume));

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.5f));
    EXPECT_TRUE(pending(firstRender));
    EXPECT_TRUE(pending(secondRender));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(pending(firstRender));
    EXPECT_TRUE(pending(secondRender));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_FALSE(pending(firstRender));
    EXPECT_FALSE(pending(secondRender));
    const float midpointDb = 20.0f * std::log10(0.5f * (1.0f + DbToLinear(-12.0f)));
    EXPECT_NEAR(midpointDb, engine->params.ResolvedValue(
                                firstRender,
                                harken::kTargetVoiceVolume), 1.0e-5f);
    EXPECT_NEAR(midpointDb, engine->params.ResolvedValue(
                                secondRender,
                                harken::kTargetVoiceVolume), 1.0e-5f);
}

TEST(VariableCurrentSlice,
     EqualGlobalWritePublishesOverriddenVoiceAndReleasesSliceOwnership) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const VnVoiceHandle voice =
        PlayDc(engine, harkentest::ObjectId(144), VN_BUS_MASTER, emitter);
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, render, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());

    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_FLOAT_EQ(-12.0f, engine->params.ResolvedValue(
                                render, harken::kTargetVoiceVolume));
    harken::VariableOutcomeRecord discarded{};
    while (engine->variables.PopOutcome(&discarded)) {}
    const std::uint32_t freeRows = engine->variableSlices.FreeRowCount();
    const std::uint32_t freeTargets =
        engine->variableSlices.FreeTargetCapacity();
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t commits = engine->params.CommitCount();
    const std::uint64_t issuedBefore = engine->logicCommits.IssuedCount();
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.0f));
    const std::uint64_t durableTicket = engine->logicCommits.IssuedCount();
    ASSERT_EQ(issuedBefore + 1u, durableTicket);
    EXPECT_TRUE(engine->variables.HasPendingWriteForVoiceTarget(
        engine->params, render, harken::kTargetVoiceVolume));
    EXPECT_FALSE(engine->params.PendingPublication(
        render, harken::kTargetVoiceVolume));

    for (std::uint32_t block = 0;
         block < 32 && engine->variables.PendingCount() != 0; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    }
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_GE(engine->commands.PublishedHead(), commandHead);
    EXPECT_GT(engine->params.CommitCount(), commits);
    EXPECT_FLOAT_EQ(0.0f, engine->params.ResolvedValue(
                                render, harken::kTargetVoiceVolume));
    harken::VariableOutcomeRecord outcome{};
    ASSERT_TRUE(engine->variables.PopOutcome(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_NE(0u, outcome.bankVersionStableOrdinal);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
}

TEST(VariableCurrentSlice, RejectedEmitterSetDoesNotActivateItsScopeBank) {
    VnConfig config = MakeNullConfig(1, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_FALSE(engine->variables.EmitterBankActiveForTests(emitter));
    const harken::VariableCallObservation observation =
        harken::ObserveVariableCallAt(
            engine->variables.CurrentWorldGeneration(),
            engine->variables.CurrentTransportEpoch(), kBlockFrames,
            engine->timelinePool.WindowStartFrame(),
            engine->timelinePool.PublishCutoffFrame());

    EXPECT_EQ(VN_INVALID_ARG,
              engine->variables.AcceptEmitter(
                  emitter, kVariable, 2.0f, observation,
                  engine->emitters, engine->params)
                  .result);
    EXPECT_FALSE(engine->variables.EmitterBankActiveForTests(emitter));
    EXPECT_EQ(VN_NO_VARIABLE,
              engine->variables.AcceptEmitter(
                  emitter, harkentest::OffsetObjectId(kVariable, 1u), 0.5f, observation,
                  engine->emitters, engine->params)
                  .result);
    EXPECT_FALSE(engine->variables.EmitterBankActiveForTests(emitter));

    EXPECT_EQ(VN_OK,
              engine->variables.AcceptEmitter(
                  emitter, kVariable, 0.5f, observation,
                  engine->emitters, engine->params)
                  .result);
    EXPECT_TRUE(engine->variables.EmitterBankActiveForTests(emitter));
}

TEST(VariableCurrentSlice,
     LaterGlobalWriteReevaluatesAfterItsPendingEmitterPredecessor) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const VnVoiceHandle voice =
        PlayDc(engine, harkentest::ObjectId(146), VN_BUS_MASTER, emitter);
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, render, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());

    harken::VariableCallObservation setObservation =
        harken::ObserveVariableCallAt(
            engine->variables.CurrentWorldGeneration(),
            engine->variables.CurrentTransportEpoch(), kBlockFrames,
            engine->timelinePool.WindowStartFrame(),
            engine->timelinePool.PublishCutoffFrame());
    setObservation.requestedEffectiveFrame =
        (engine->mixer.BlocksRendered() + 1u) * kBlockFrames;
    harken::VariableCallObservation globalObservation = setObservation;
    globalObservation.requestedEffectiveFrame += kBlockFrames;
    const harken::VariableAcceptResult set =
        engine->variables.AcceptEmitter(emitter, kVariable, 1.0f,
                                        setObservation, engine->emitters,
                                        engine->params);
    ASSERT_EQ(VN_OK, set.result);
    const harken::VariableAcceptResult globalWrite =
        engine->variables.AcceptGlobal(
            kVariable, 0.0f, globalObservation, engine->params,
            engine->bindings, engine->busTargets);
    ASSERT_EQ(VN_OK, globalWrite.result);
    ASSERT_LT(set.ticket.sequence, globalWrite.ticket.sequence);
    EXPECT_TRUE(engine->variables.HasPendingWriteForVoiceTarget(
        engine->params, render, harken::kTargetVoiceVolume));

    bool sawSetOutcome = false;
    bool sawGlobalOutcome = false;
    for (std::uint32_t block = 0;
         block < 64 && engine->variables.PendingCount() != 0; ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        harken::VariableOutcomeRecord outcome{};
        while (engine->variables.PopOutcome(&outcome)) {
            if (outcome.acceptedTicket.sequence == set.ticket.sequence) {
                EXPECT_FALSE(sawSetOutcome);
                sawSetOutcome = true;
                float scoped = 0.0f;
                ASSERT_TRUE(engine->variables.CurrentValue(
                    emitter, kVariable, &scoped));
                EXPECT_FLOAT_EQ(1.0f, scoped);
                EXPECT_EQ(1u, engine->variables.PendingCount());
                EXPECT_TRUE(
                    engine->variables.HasPendingWriteForVoiceTarget(
                        engine->params, render,
                        harken::kTargetVoiceVolume));
            } else if (outcome.acceptedTicket.sequence ==
                       globalWrite.ticket.sequence) {
                EXPECT_TRUE(sawSetOutcome);
                EXPECT_FALSE(sawGlobalOutcome);
                sawGlobalOutcome = true;
            }
        }
    }

    ASSERT_TRUE(sawSetOutcome);
    ASSERT_TRUE(sawGlobalOutcome);
    ASSERT_EQ(0u, engine->variables.PendingCount());
    EXPECT_FALSE(engine->variables.HasPendingWriteForVoiceTarget(
        engine->params, render, harken::kTargetVoiceVolume));
    float global = 0.0f;
    float scoped = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &global));
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, kVariable, &scoped));
    EXPECT_FLOAT_EQ(0.0f, global);
    EXPECT_FLOAT_EQ(global, scoped);
    ASSERT_NE(nullptr, engine->voices.Resolve(render));
    EXPECT_FLOAT_EQ(1.0f, engine->voices.Resolve(render)->gain.target);
}

TEST(VariableCurrentSlice,
     EmitterDestroyWhileSliceIsPreparedReleasesTheWholeClaimImmediately) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const VnVoiceHandle first = PlayDc(engine, harkentest::ObjectId(40), VN_BUS_MASTER, emitter);
    const VnVoiceHandle second = PlayDc(engine, harkentest::ObjectId(41), VN_BUS_MASTER, emitter);
    const VnVoiceHandle firstRender = harkentest::RenderHandle(engine, first);
    const VnVoiceHandle secondRender = harkentest::RenderHandle(engine, second);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, secondRender, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, firstRender, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    const std::uint32_t freeRows = engine->variableSlices.FreeRowCount();
    const std::uint32_t freeTargets =
        engine->variableSlices.FreeTargetCapacity();
    const std::uint64_t issuedBefore = engine->logicCommits.IssuedCount();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, 1.0f));
    const std::uint64_t durableTicket = engine->logicCommits.IssuedCount();
    ASSERT_EQ(issuedBefore + 1u, durableTicket);
    engine->nullBackend.Pump(8);
    engine->variables.BeginService(*engine, engine->mixer.BlocksRendered(),
                                   kBlockFrames);
    const std::uint64_t expectedActualFrame =
        engine->mixer.BlocksRendered() * kBlockFrames;
    ASSERT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    ASSERT_EQ(freeTargets - 2u,
              engine->variableSlices.FreeTargetCapacity());
    ASSERT_TRUE(engine->params.ForeignClaim(firstRender,
                                            harken::kTargetVoiceVolume));
    ASSERT_TRUE(engine->params.ForeignClaim(secondRender,
                                            harken::kTargetVoiceVolume));

    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
    EXPECT_FALSE(engine->params.ForeignClaim(firstRender,
                                             harken::kTargetVoiceVolume));
    EXPECT_FALSE(engine->params.ForeignClaim(secondRender,
                                             harken::kTargetVoiceVolume));
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(1u, engine->variables.TerminationCount());
    float current = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, kVariable,
                                               &current));
    EXPECT_FLOAT_EQ(0.0f, current);
    harken::VariableOutcomeRecord outcome{};
    ASSERT_TRUE(engine->variables.PopOutcome(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::EmitterDestroyed,
              outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(0u, outcome.relatedTicket.sequence);
    EXPECT_EQ(expectedActualFrame, outcome.actualFrame);
    EXPECT_EQ(0u, outcome.bankVersionStableOrdinal);
    EXPECT_FALSE(engine->variables.PopOutcome(&outcome));
    EXPECT_FLOAT_EQ(1.0f,
                    engine->voices.Resolve(firstRender)->gain.target);
    EXPECT_FLOAT_EQ(1.0f,
                    engine->voices.Resolve(secondRender)->gain.target);
}

TEST(VariableCurrentSlice,
     EmitterDestroyBeforeAudioClaimWaitsForTheStrictTombstoneFence) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const VnVoiceHandle first = PlayDc(engine, harkentest::ObjectId(42), VN_BUS_MASTER, emitter);
    const VnVoiceHandle second = PlayDc(engine, harkentest::ObjectId(43), VN_BUS_MASTER, emitter);
    const VnVoiceHandle firstRender = harkentest::RenderHandle(engine, first);
    const VnVoiceHandle secondRender = harkentest::RenderHandle(engine, second);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, firstRender, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, secondRender, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    const std::uint32_t freeRows = engine->variableSlices.FreeRowCount();
    const std::uint32_t freeTargets =
        engine->variableSlices.FreeTargetCapacity();
    const std::uint64_t issuedBefore = engine->logicCommits.IssuedCount();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, 1.0f));
    const std::uint64_t durableTicket = engine->logicCommits.IssuedCount();
    ASSERT_EQ(issuedBefore + 1u, durableTicket);
    MakeAcceptedWriteDue(engine);
    ASSERT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    ASSERT_EQ(freeTargets - 2u,
              engine->variableSlices.FreeTargetCapacity());
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    EXPECT_EQ(1u, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->variables.TerminationCount());
    EXPECT_TRUE(engine->params.ForeignClaim(firstRender,
                                            harken::kTargetVoiceVolume));
    EXPECT_TRUE(engine->params.ForeignClaim(secondRender,
                                            harken::kTargetVoiceVolume));

    for (std::uint32_t block = 0;
         block < 16 && engine->variables.PendingCount() != 0; ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(1u, engine->variables.TerminationCount());
    EXPECT_FALSE(engine->params.ForeignClaim(firstRender,
                                             harken::kTargetVoiceVolume));
    EXPECT_FALSE(engine->params.ForeignClaim(secondRender,
                                             harken::kTargetVoiceVolume));
    EXPECT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets - 2u,
              engine->variableSlices.FreeTargetCapacity());
    const std::uint64_t expectedActualFrame =
        engine->mixer.BlocksRendered() * kBlockFrames;
    harken::VariableOutcomeRecord outcome{};
    ASSERT_TRUE(engine->variables.PopOutcome(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::EmitterDestroyed,
              outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(0u, outcome.relatedTicket.sequence);
    EXPECT_EQ(expectedActualFrame, outcome.actualFrame);
    EXPECT_EQ(0u, outcome.bankVersionStableOrdinal);
    EXPECT_FALSE(engine->variables.PopOutcome(&outcome));

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
    EXPECT_FLOAT_EQ(1.0f,
                    engine->voices.Resolve(firstRender)->gain.target);
    EXPECT_FLOAT_EQ(1.0f,
                    engine->voices.Resolve(secondRender)->gain.target);
}

TEST(VariableCurrentSlice,
     EmitterDestroyBeforeAudioClaimReportsTheFirstTombstoneBlock) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const VnVoiceHandle voice =
        PlayDc(engine, harkentest::ObjectId(143), VN_BUS_MASTER, emitter);
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, render, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, 1.0f));
    const std::uint64_t durableTicket = engine->logicCommits.IssuedCount();
    MakeAcceptedWriteDue(engine);
    const harken::VariableSliceRef ref =
        engine->variables.SliceForTicketForTests(durableTicket);
    ASSERT_TRUE(ref.Valid());
    ASSERT_EQ(harken::VariableSliceState::PageCommitted,
              engine->variableSlices.State(ref));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));

    for (std::uint32_t block = 0;
         block < 16 &&
         engine->variableSlices.AppliedBlockSequence(ref) == 0;
         ++block) {
        engine->nullBackend.Pump(1);
    }
    const std::uint64_t tombstoneBlock =
        engine->variableSlices.AppliedBlockSequence(ref);
    ASSERT_NE(0u, tombstoneBlock);
    EXPECT_EQ(harken::VariableSliceState::Invalidated,
              engine->variableSlices.State(ref));
    EXPECT_EQ(1u, engine->variables.PendingCount());

    engine->nullBackend.Pump(2);
    ASSERT_GT(engine->mixer.BlocksRendered(), tombstoneBlock);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(0u, engine->variables.PendingCount());
    harken::VariableOutcomeRecord outcome{};
    ASSERT_TRUE(engine->variables.PopOutcome(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::EmitterDestroyed,
              outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(tombstoneBlock * kBlockFrames, outcome.actualFrame);
    EXPECT_LT(outcome.actualFrame,
              engine->mixer.BlocksRendered() * kBlockFrames);
}

TEST(VariableCurrentSlice,
     EmitterDestroyAtAudioClaimAppliesWholeThenFallsBackToGlobal) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    config.maxBindings = 4;
    config.voiceBindingInstanceSlots = 2;
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const VnVoiceHandle first = PlayDc(engine, harkentest::ObjectId(44), VN_BUS_MASTER, emitter);
    const VnVoiceHandle second = PlayDc(engine, harkentest::ObjectId(45), VN_BUS_MASTER, emitter);
    const VnVoiceHandle firstRender = harkentest::RenderHandle(engine, first);
    const VnVoiceHandle secondRender = harkentest::RenderHandle(engine, second);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, firstRender, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, secondRender, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.gainDb = 0.0f;
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(
                  first, 0, VN_EFFECT_GAIN,
                  reinterpret_cast<const VnEffectDesc*>(&gain)));
    VnBindingDesc effect{};
    effect.structSize = sizeof(effect);
    effect.variableId = kVariable;
    effect.targetScopeKind = VN_TARGET_SCOPE_VOICE_EFFECT_PARAM;
    effect.targetId = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    effect.curve.structSize = sizeof(effect.curve);
    effect.curve.pointCount = 2;
    effect.curve.points[0] = {0.0f, 0.0f};
    effect.curve.points[1] = {1.0f, -6.0f};
    effect.curve.handles[0] = harkentest::EndpointHandles(
        effect.curve.points[0], effect.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &effect));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(first, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    const std::uint32_t freeRows = engine->variableSlices.FreeRowCount();
    const std::uint32_t freeTargets =
        engine->variableSlices.FreeTargetCapacity();
    const std::uint64_t issuedBefore = engine->logicCommits.IssuedCount();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, 1.0f));
    const std::uint64_t durableTicket = engine->logicCommits.IssuedCount();
    ASSERT_EQ(issuedBefore + 1u, durableTicket);
    MakeAcceptedWriteDue(engine);
    const std::uint32_t firstGeneration =
        static_cast<std::uint32_t>(firstRender >> 32);
    ASSERT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    ASSERT_EQ(freeTargets - 3u,
              engine->variableSlices.FreeTargetCapacity());
    ASSERT_TRUE(engine->effectParamTargets.ForeignClaim(
        harken::EffectParamTargetStore::Scope::Voice, firstRender,
        firstGeneration, 0, harken::kGainParamGainDb));
    AudioClaimBarrier barrier{};
    engine->variableSlices.SetAfterAudioClaimHookForTest(
        BlockAfterAudioClaim, &barrier);
    std::atomic<bool> workerDone{false};
    std::thread audio([&] {
        for (std::uint32_t block = 0;
             block < 16 &&
             !barrier.entered.load(std::memory_order_acquire);
             ++block) {
            engine->nullBackend.Pump(1);
        }
        workerDone.store(true, std::memory_order_release);
    });
    while (!barrier.entered.load(std::memory_order_acquire) &&
           !workerDone.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    const bool entered = barrier.entered.load(std::memory_order_acquire);
    const VnResult destroyResult =
        entered ? Vn_DestroyEmitter(emitter) : VN_DEVICE_ERROR;
    barrier.release.store(true, std::memory_order_release);
    audio.join();
    engine->variableSlices.SetAfterAudioClaimHookForTest(nullptr, nullptr);

    ASSERT_TRUE(entered);
    ASSERT_EQ(VN_OK, destroyResult);
    EXPECT_EQ(harken::VariableSliceState::SliceApplied,
              engine->variableSlices.State(barrier.ref));
    const std::uint64_t appliedBlock =
        engine->variableSlices.AppliedBlockSequence(barrier.ref);
    EXPECT_NE(0u, appliedBlock);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f),
                    engine->voices.Resolve(firstRender)->gain.target);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f),
                    engine->voices.Resolve(secondRender)->gain.target);
    const harken::VoiceRenderContext* firstState =
        engine->voices.Resolve(firstRender);
    ASSERT_NE(nullptr, firstState);
    ASSERT_EQ(VN_EFFECT_GAIN, firstState->fxSlotEffectRef[0].type);
    const auto* gainState =
        static_cast<const harken::GainState*>(firstState->fxSlots[0].state);
    ASSERT_NE(nullptr, gainState);
    EXPECT_FLOAT_EQ(DbToLinear(-6.0f), gainState->targetGainLinear);
    EXPECT_EQ(1u, engine->variables.PendingCount());
    EXPECT_TRUE(engine->params.ForeignClaim(firstRender,
                                            harken::kTargetVoiceVolume));
    EXPECT_TRUE(engine->params.ForeignClaim(secondRender,
                                            harken::kTargetVoiceVolume));
    float current = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, kVariable,
                                               &current));
    EXPECT_FLOAT_EQ(0.0f, current);

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(0.0f, engine->params.ResolvedValue(
                              firstRender, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(0.0f, engine->params.ResolvedValue(
                              secondRender, harken::kTargetVoiceVolume));
    EXPECT_TRUE(engine->params.PendingPublication(
        firstRender, harken::kTargetVoiceVolume));
    EXPECT_TRUE(engine->params.PendingPublication(
        secondRender, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(0.0f, engine->effectParamTargets.Reduce(
                                  harken::EffectParamTargetStore::Scope::Voice,
                                  firstRender, 0,
                                  harken::kGainParamGainDb));
    EXPECT_FLOAT_EQ(-6.0f, engine->effectParamTargets.LastCommitted(
                                   harken::EffectParamTargetStore::Scope::Voice,
                                   firstRender, 0,
                                   harken::kGainParamGainDb));
    EXPECT_TRUE(engine->effectParamTargets.PendingPublication(
        harken::EffectParamTargetStore::Scope::Voice, firstRender, 0,
        harken::kGainParamGainDb));
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(1u, engine->variables.TerminationCount());
    EXPECT_FALSE(engine->params.ForeignClaim(firstRender,
                                             harken::kTargetVoiceVolume));
    EXPECT_FALSE(engine->params.ForeignClaim(secondRender,
                                             harken::kTargetVoiceVolume));
    EXPECT_FALSE(engine->effectParamTargets.ForeignClaim(
        harken::EffectParamTargetStore::Scope::Voice, firstRender,
        firstGeneration, 0, harken::kGainParamGainDb));
    EXPECT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets - 3u,
              engine->variableSlices.FreeTargetCapacity());
    harken::VariableOutcomeRecord outcome{};
    ASSERT_TRUE(engine->variables.PopOutcome(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::EmitterDestroyed,
              outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(0u, outcome.relatedTicket.sequence);
    EXPECT_EQ(appliedBlock * kBlockFrames, outcome.actualFrame);
    EXPECT_EQ(0u, outcome.bankVersionStableOrdinal);
    EXPECT_FALSE(engine->variables.PopOutcome(&outcome));

    engine->nullBackend.Pump(1);
    EXPECT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets - 3u,
              engine->variableSlices.FreeTargetCapacity());
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f),
                    engine->voices.Resolve(firstRender)->gain.target);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f),
                    engine->voices.Resolve(secondRender)->gain.target);
    EXPECT_FLOAT_EQ(DbToLinear(-6.0f), gainState->targetGainLinear);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f),
                    engine->voices.Resolve(firstRender)->gain.target);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f),
                    engine->voices.Resolve(secondRender)->gain.target);
    EXPECT_FLOAT_EQ(DbToLinear(-6.0f), gainState->targetGainLinear);
    engine->nullBackend.Pump(1);
    EXPECT_FLOAT_EQ(1.0f,
                    engine->voices.Resolve(firstRender)->gain.target);
    EXPECT_FLOAT_EQ(1.0f,
                    engine->voices.Resolve(secondRender)->gain.target);
    EXPECT_FLOAT_EQ(1.0f, gainState->targetGainLinear);
}

TEST(VariableCurrentSlice,
     EmitterDestroyAfterSliceAppliedKeepsAppliedOutcomeWithoutBankRevival) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const VnVoiceHandle first = PlayDc(engine, harkentest::ObjectId(46), VN_BUS_MASTER, emitter);
    const VnVoiceHandle second = PlayDc(engine, harkentest::ObjectId(47), VN_BUS_MASTER, emitter);
    const VnVoiceHandle firstRender = harkentest::RenderHandle(engine, first);
    const VnVoiceHandle secondRender = harkentest::RenderHandle(engine, second);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, firstRender, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, secondRender, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    const std::uint32_t freeRows = engine->variableSlices.FreeRowCount();
    const std::uint32_t freeTargets =
        engine->variableSlices.FreeTargetCapacity();
    const std::uint64_t issuedBefore = engine->logicCommits.IssuedCount();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, 1.0f));
    const std::uint64_t durableTicket = engine->logicCommits.IssuedCount();
    ASSERT_EQ(issuedBefore + 1u, durableTicket);
    MakeAcceptedWriteDue(engine);
    const AudioClaimWaitResult observation = WaitForAudioClaim(engine);
    ASSERT_EQ(1u, observation.callCount);
    EXPECT_EQ(harken::VariableSliceState::SliceApplied,
              engine->variableSlices.State(observation.ref));
    const std::uint64_t appliedBlock =
        engine->variableSlices.AppliedBlockSequence(observation.ref);
    EXPECT_NE(0u, appliedBlock);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f),
                    engine->voices.Resolve(firstRender)->gain.target);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f),
                    engine->voices.Resolve(secondRender)->gain.target);
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(0.0f, engine->params.ResolvedValue(
                              firstRender, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(0.0f, engine->params.ResolvedValue(
                              secondRender, harken::kTargetVoiceVolume));
    EXPECT_TRUE(engine->params.PendingPublication(
        firstRender, harken::kTargetVoiceVolume));
    EXPECT_TRUE(engine->params.PendingPublication(
        secondRender, harken::kTargetVoiceVolume));

    EXPECT_EQ(1u, engine->variables.AppliedCount());
    EXPECT_EQ(0u, engine->variables.TerminationCount());
    EXPECT_FALSE(engine->params.ForeignClaim(firstRender,
                                             harken::kTargetVoiceVolume));
    EXPECT_FALSE(engine->params.ForeignClaim(secondRender,
                                             harken::kTargetVoiceVolume));
    float current = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, kVariable,
                                               &current));
    EXPECT_FLOAT_EQ(0.0f, current);
    EXPECT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets - 2u,
              engine->variableSlices.FreeTargetCapacity());
    harken::VariableOutcomeRecord outcome{};
    ASSERT_TRUE(engine->variables.PopOutcome(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(0u, outcome.relatedTicket.sequence);
    EXPECT_EQ(appliedBlock * kBlockFrames, outcome.actualFrame);
    EXPECT_EQ(1u, outcome.bankVersionStableOrdinal);
    EXPECT_FALSE(engine->variables.PopOutcome(&outcome));

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
    for (std::uint32_t block = 0;
         block < 16 &&
         (engine->voices.Resolve(firstRender)->gain.target != 1.0f ||
          engine->voices.Resolve(secondRender)->gain.target != 1.0f);
         ++block) {
        engine->nullBackend.Pump(1);
    }
    EXPECT_FLOAT_EQ(1.0f,
                    engine->voices.Resolve(firstRender)->gain.target);
    EXPECT_FLOAT_EQ(1.0f,
                    engine->voices.Resolve(secondRender)->gain.target);
}

TEST(VariableCurrentSlice,
     EmitterDetachGathersBuiltinsAndVoiceEffectIntoTheNextGlobalSlice) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    config.maxBindings = 8;
    config.maxBindingAttachments = 4;
    config.voiceBindingInstanceSlots = 4;
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const VnVoiceHandle voice =
        PlayDc(engine, harkentest::ObjectId(48), VN_BUS_MASTER, emitter);
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, render, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, render, harken::kTargetVoiceLowpassCutoff,
        LinearCurve(0.0f, 20000.0f, 1.0f, 8000.0f)));

    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.gainDb = 0.0f;
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(
                  voice, 0, VN_EFFECT_GAIN,
                  reinterpret_cast<const VnEffectDesc*>(&gain)));
    VnBindingDesc effect{};
    effect.structSize = sizeof(effect);
    effect.variableId = kVariable;
    effect.targetScopeKind = VN_TARGET_SCOPE_VOICE_EFFECT_PARAM;
    effect.targetId = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    effect.curve.structSize = sizeof(effect.curve);
    effect.curve.pointCount = 2;
    effect.curve.points[0] = {0.0f, 0.0f};
    effect.curve.points[1] = {1.0f, -6.0f};
    effect.curve.handles[0] = harkentest::EndpointHandles(
        effect.curve.points[0], effect.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &effect));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(voice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    const std::uint32_t freeRows = engine->variableSlices.FreeRowCount();
    const std::uint32_t freeTargets =
        engine->variableSlices.FreeTargetCapacity();

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.5f));
    MakeAcceptedWriteDue(engine);
    ASSERT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    ASSERT_EQ(freeTargets - 3u,
              engine->variableSlices.FreeTargetCapacity());
    EXPECT_TRUE(engine->params.ForeignClaim(
        render, harken::kTargetVoiceVolume));
    EXPECT_TRUE(engine->params.ForeignClaim(
        render, harken::kTargetVoiceLowpassCutoff));
    const std::uint32_t generation =
        static_cast<std::uint32_t>(render >> 32);
    EXPECT_TRUE(engine->effectParamTargets.ForeignClaim(
        harken::EffectParamTargetStore::Scope::Voice, render, generation, 0,
        harken::kGainParamGainDb));

    const AudioClaimWaitResult observation = WaitForAudioClaim(engine);
    ASSERT_EQ(1u, observation.callCount);
    EXPECT_EQ(harken::VariableSliceState::SliceApplied,
              engine->variableSlices.State(observation.ref));
    harken::VoiceRenderContext* voiceState = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, voiceState);
    const float midpointGain = 0.5f * (1.0f + DbToLinear(-12.0f));
    EXPECT_NEAR(midpointGain, voiceState->gain.target, 1.0e-6f);
    EXPECT_NEAR(14000.0f, std::exp2(voiceState->lpfGlide.targetLog2Hz),
                1.0f);
    ASSERT_EQ(VN_EFFECT_GAIN, voiceState->fxSlotEffectRef[0].type);
    const auto* gainState =
        static_cast<const harken::GainState*>(voiceState->fxSlots[0].state);
    ASSERT_NE(nullptr, gainState);
    EXPECT_FLOAT_EQ(DbToLinear(-3.0f), gainState->targetGainLinear);

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FALSE(engine->params.ForeignClaim(
        render, harken::kTargetVoiceVolume));
    EXPECT_FALSE(engine->params.ForeignClaim(
        render, harken::kTargetVoiceLowpassCutoff));
    EXPECT_FALSE(engine->effectParamTargets.ForeignClaim(
        harken::EffectParamTargetStore::Scope::Voice, render, generation, 0,
        harken::kGainParamGainDb));
    EXPECT_NEAR(20.0f * std::log10(midpointGain), engine->params.LastCommitted(
                    render, harken::kTargetVoiceVolume), 1.0e-5f);
    EXPECT_FLOAT_EQ(14000.0f,
                    engine->params.LastCommitted(
                        render, harken::kTargetVoiceLowpassCutoff));
    EXPECT_FLOAT_EQ(-3.0f,
                    engine->effectParamTargets.LastCommitted(
                        harken::EffectParamTargetStore::Scope::Voice, render,
                        0, harken::kGainParamGainDb));
    float current = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, kVariable,
                                               &current));
    EXPECT_FLOAT_EQ(0.5f, current);
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(0.5f, current);
}

TEST(VariableCurrentSlice,
     NaturalVoiceEndRetiresBuiltinAndEffectOwnersUntilExactRelease) {
    VnConfig config = MakeNullConfig(1, kBlockFrames, kSampleRate);
    config.fxSlotsPerVoice = 1;
    config.delayEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> samples(kBlockFrames, 0.05f);
    const VnPCMDesc pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(57), &pcm));
    VnVoiceHandle voice = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(57));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    ASSERT_NE(0u, render);

    VnFeedbackDelayDesc delay{};
    delay.structSize = sizeof(delay);
    delay.delayMs = 10.0f;
    delay.feedback = 0.25f;
    delay.wet = 0.75f;
    delay.dry = 1.0f;
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(
                  voice, 0, VN_EFFECT_FEEDBACK_DELAY,
                  reinterpret_cast<const VnEffectDesc*>(&delay)));
    ASSERT_TRUE(engine->params.SetApiDirect(
        render, harken::kTargetVoiceVolume, -6.0f));
    const harken::VariableSliceRef owner{20, 301};
    const std::uint32_t voiceRevision = engine->params.TargetRevision(
        render, harken::kTargetVoiceVolume);
    ASSERT_TRUE(engine->params.TryClaim(
        render, harken::kTargetVoiceVolume, owner, voiceRevision));
    const std::uint32_t generation =
        static_cast<std::uint32_t>(render >> 32);
    std::uint32_t effectRevision = 0;
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Voice, render, generation, 0,
        harken::kDelayParamWet, VN_EFFECT_FEEDBACK_DELAY, &effectRevision));
    ASSERT_TRUE(engine->effectParamTargets.TryClaim(
        harken::EffectParamTargetStore::Scope::Voice, render, generation, 0,
        harken::kDelayParamWet, owner, effectRevision));

    for (std::uint32_t block = 0;
         block < 64 && engine->voices.Resolve(render) != nullptr; ++block) {
        engine->nullBackend.Pump(1);
    }
    EXPECT_EQ(nullptr, engine->voices.Resolve(render));
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(voice).kind);
    EXPECT_EQ(0u, engine->params.RetiringTargetCountForTests());
    EXPECT_EQ(0u,
              engine->effectParamTargets.RetiringRecordCountForTests());
    EXPECT_TRUE(engine->params.ForeignClaim(
        render, harken::kTargetVoiceVolume));
    EXPECT_TRUE(engine->effectParamTargets.ForeignClaim(
        harken::EffectParamTargetStore::Scope::Voice, render, generation, 0,
        harken::kDelayParamWet));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Stale,
              engine->ledger.Classify(voice).kind);
    ASSERT_EQ(1u, engine->params.RetiringTargetCountForTests());
    EXPECT_EQ(0u, engine->effectParamTargets.LiveRecordCount());
    ASSERT_EQ(1u,
              engine->effectParamTargets.RetiringRecordCountForTests());

    float resolved = 0.0f;
    std::uint32_t rampMs = 0;
    std::uint32_t revision = 0;
    ASSERT_TRUE(engine->params.SnapshotForClaim(
        render, harken::kTargetVoiceVolume, owner, &resolved, &rampMs,
        &revision));
    EXPECT_FLOAT_EQ(-6.0f, resolved);
    EXPECT_EQ(voiceRevision, revision);
    std::uint32_t effectType = 0;
    ASSERT_TRUE(engine->effectParamTargets.SnapshotForClaim(
        harken::EffectParamTargetStore::Scope::Voice, render, generation, 0,
        harken::kDelayParamWet, owner, &resolved, &revision, &effectType));
    EXPECT_FLOAT_EQ(0.75f, resolved);
    EXPECT_EQ(effectRevision, revision);
    EXPECT_EQ(VN_EFFECT_FEEDBACK_DELAY, effectType);
    for (const std::uint32_t unclaimed : {
             harken::kDelayParamTimeMs, harken::kDelayParamFeedback,
             harken::kDelayParamDry}) {
        EXPECT_FALSE(engine->effectParamTargets.ForeignClaim(
            harken::EffectParamTargetStore::Scope::Voice, render, generation,
            0, unclaimed));
        EXPECT_FALSE(engine->effectParamTargets.SnapshotForClaim(
            harken::EffectParamTargetStore::Scope::Voice, render, generation,
            0, unclaimed, owner, &resolved, &revision, &effectType));
    }

    engine->params.ReleaseClaim(render, harken::kTargetVoiceVolume, owner);
    EXPECT_EQ(0u, engine->params.RetiringTargetCountForTests());
    EXPECT_FALSE(engine->params.ForeignClaim(
        render, harken::kTargetVoiceVolume));
    EXPECT_FALSE(engine->params.SnapshotForClaim(
        render, harken::kTargetVoiceVolume, owner, &resolved, &rampMs,
        &revision));
    EXPECT_EQ(1u,
              engine->effectParamTargets.RetiringRecordCountForTests());
    engine->effectParamTargets.MarkSliceCommitted(
        harken::EffectParamTargetStore::Scope::Voice, render, generation, 0,
        harken::kDelayParamWet, owner, 0.75f);
    EXPECT_EQ(0u,
              engine->effectParamTargets.RetiringRecordCountForTests());
    EXPECT_FALSE(engine->effectParamTargets.ForeignClaim(
        harken::EffectParamTargetStore::Scope::Voice, render, generation, 0,
        harken::kDelayParamWet));
    EXPECT_FALSE(engine->effectParamTargets.SnapshotForClaim(
        harken::EffectParamTargetStore::Scope::Voice, render, generation, 0,
        harken::kDelayParamWet, owner, &resolved, &revision, &effectType));
}

TEST(VariableCurrentSlice,
     RetiringVoiceOwnerSurvivesDirectDeferredAndParkReturnReuse) {
    {
        EngineSession session(MakeNullConfig(1, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const VnVoiceHandle oldVoice =
            PlayDc(engine, harkentest::ObjectId(49), VN_BUS_MASTER);
        const VnVoiceHandle oldRender =
            harkentest::RenderHandle(engine, oldVoice);
        const harken::VariableSliceRef oldOwner{10, 201};
        ClaimVoiceVolumeForRetirement(engine, oldRender, oldOwner);
        ASSERT_EQ(VN_OK, Vn_Stop(oldVoice, 0));
        for (std::uint32_t step = 0;
             step < 8 &&
             engine->params.RetiringTargetCountForTests() == 0;
             ++step) {
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        ASSERT_EQ(1u, engine->params.RetiringTargetCountForTests());
        const VnVoiceHandle newVoice =
            PlayDc(engine, harkentest::ObjectId(50), VN_BUS_MASTER);
        const VnVoiceHandle newRender =
            harkentest::RenderHandle(engine, newVoice);
        ExpectRetiringVoiceOwnerIsolatedAfterReuse(
            engine, oldRender, newRender, oldOwner, {11, 201}, 0.0f);
    }
    {
        VnConfig config = MakeNullConfig(1, kBlockFrames, kSampleRate);
        config.voiceStealPolicy = 2;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        static const std::vector<float> samples(kSampleRate, 0.05f);
        const VnPCMDesc pcm = MonoDesc(samples, kSampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(51), &pcm));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(52), &pcm));
        VnVoiceHandle oldVoice = 0;
        const VnPlayDesc oldPlay = MakePlayDesc(harkentest::ObjectId(51));
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&oldPlay, &oldVoice));
        engine->nullBackend.Pump(1);
        const VnVoiceHandle oldRender =
            harkentest::RenderHandle(engine, oldVoice);
        const harken::VariableSliceRef oldOwner{12, 202};
        ClaimVoiceVolumeForRetirement(engine, oldRender, oldOwner);

        VnVoiceHandle incoming = 0;
        const VnPlayDesc incomingPlay = MakePlayDesc(harkentest::ObjectId(52));
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&incomingPlay, &incoming));
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        ASSERT_EQ(1u, engine->params.RetiringTargetCountForTests());
        const VnVoiceHandle newRender =
            harkentest::RenderHandle(engine, incoming);
        ASSERT_NE(nullptr, engine->voices.Resolve(newRender));
        ExpectRetiringVoiceOwnerIsolatedAfterReuse(
            engine, oldRender, newRender, oldOwner, {13, 202}, 0.0f);
    }
    {
        VnConfig config = MakeNullConfig(1, kBlockFrames, kSampleRate);
        config.maxParkedVoices = 1;
        config.audibilityParkEnabled = 1;
        config.parkThresholdDb = -40.0f;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        static const std::vector<float> samples(kSampleRate, 0.05f);
        const VnPCMDesc pcm = MonoDesc(samples, kSampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(53), &pcm));
        const VnAttenuationDesc attenuation =
            harkentest::MakeAttenuationDesc(1.0f, 100.0f);
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(54), &attenuation));
        const auto listenerAt = [](float x) {
            ASSERT_EQ(VN_OK,
                      Vn_SetListener(
                          harkentest::Vec3(x, 0.0f, 0.0f),
                          harkentest::Vec3(0.0f, 0.0f, -1.0f),
                          harkentest::Vec3(0.0f, 1.0f, 0.0f)));
        };
        const VnPlayDesc play =
            harkentest::MakePlayDesc3D(harkentest::ObjectId(53), 100.0f, 0.0f, 0.0f);
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, harkentest::ObjectId(54)));
        listenerAt(99.0f);
        engine->nullBackend.Pump(1);
        const VnVoiceHandle oldRender =
            harkentest::RenderHandle(engine, voice);
        const harken::VariableSliceRef oldOwner{14, 203};
        ClaimVoiceVolumeForRetirement(engine, oldRender, oldOwner);

        listenerAt(0.0f);
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
                  engine->ledger.Classify(voice).kind);
        ASSERT_EQ(1u, engine->params.RetiringTargetCountForTests());
        listenerAt(99.0f);
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        const VnVoiceHandle newRender =
            harkentest::RenderHandle(engine, voice);
        ASSERT_NE(nullptr, engine->voices.Resolve(newRender));
        ExpectRetiringVoiceOwnerIsolatedAfterReuse(
            engine, oldRender, newRender, oldOwner, {15, 203}, -6.0f);
    }
}

TEST(VariableCurrentSlice,
     AppliedOldVoiceSliceReleasesAfterDeferredRenderSlotReuse) {
    VnConfig config = MakeNullConfig(1, kBlockFrames, kSampleRate);
    config.voiceStealPolicy = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    static const std::vector<float> samples(kSampleRate, 0.05f);
    const VnPCMDesc pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(55), &pcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(56), &pcm));
    const VnPlayDesc oldPlay = MakePlayDesc(harkentest::ObjectId(55));
    VnVoiceHandle oldVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&oldPlay, &oldVoice));
    engine->nullBackend.Pump(1);
    const VnVoiceHandle oldRender =
        harkentest::RenderHandle(engine, oldVoice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, oldRender, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    const std::uint32_t freeRows = engine->variableSlices.FreeRowCount();
    const std::uint32_t freeTargets =
        engine->variableSlices.FreeTargetCapacity();
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    MakeAcceptedWriteDue(engine);
    ASSERT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    ASSERT_EQ(freeTargets - 1u,
              engine->variableSlices.FreeTargetCapacity());
    const auto pendingSlice = engine->variables.SliceForTicketForTests(
        engine->logicCommits.IssuedCount());
    ASSERT_TRUE(pendingSlice.Valid());
    ASSERT_TRUE(engine->params.ForeignClaim(
        oldRender, harken::kTargetVoiceVolume));

    const VnPlayDesc incomingPlay = MakePlayDesc(harkentest::ObjectId(56));
    VnVoiceHandle incoming = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&incomingPlay, &incoming));
    CurrentSliceScratchReset scratchReset{engine};
    engine->mixer.SetCurrentSliceScratchLimitForTest(0);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    const VnVoiceHandle newRender =
        harkentest::RenderHandle(engine, incoming);
    ASSERT_NE(nullptr, engine->voices.Resolve(newRender));
    ASSERT_EQ(static_cast<std::uint32_t>(oldRender),
              static_cast<std::uint32_t>(newRender));
    ASSERT_NE(static_cast<std::uint32_t>(oldRender >> 32),
              static_cast<std::uint32_t>(newRender >> 32));
    EXPECT_EQ(harken::VariableSliceState::PageCommitted,
              engine->variableSlices.State(pendingSlice));
    ASSERT_EQ(1u, engine->params.RetiringTargetCountForTests());
    EXPECT_FALSE(engine->params.ForeignClaim(
        newRender, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(0.0f, engine->params.LastCommitted(
                                     newRender,
                                     harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(1.0f, engine->voices.Resolve(newRender)->gain.target);

    engine->mixer.RestoreCurrentSliceScratchLimitForTest();
    const AudioClaimWaitResult observation = WaitForAudioClaim(engine);
    ASSERT_EQ(1u, observation.callCount);
    EXPECT_EQ(harken::VariableSliceState::SliceApplied,
              engine->variableSlices.State(observation.ref));
    EXPECT_EQ(1u, engine->params.RetiringTargetCountForTests());
    EXPECT_FALSE(engine->params.ForeignClaim(
        newRender, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(0.0f, engine->params.LastCommitted(
                                     newRender,
                                     harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(1.0f, engine->voices.Resolve(newRender)->gain.target);

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->params.RetiringTargetCountForTests());
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(1u, engine->variables.AppliedCount());
    EXPECT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets - 1u,
              engine->variableSlices.FreeTargetCapacity());

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
}

TEST(VariableCurrentSlice, PlayInBankAckGapRefusesInsteadOfPrimingAhead) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    const VnVoiceHandle existing = PlayDc(engine, harkentest::ObjectId(1), VN_BUS_MASTER);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, harkentest::RenderHandle(engine, existing),
        harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    static const std::vector<float> secondSamples(kSampleRate, 0.05f);
    const VnPCMDesc secondPcm = MonoDesc(secondSamples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &secondPcm));
    RegisterVoiceAttachment(harkentest::ObjectId(1), harkentest::ObjectId(2));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    MakeAcceptedWriteDue(engine);
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(2));
    VnVoiceHandle voice = 0;
    EXPECT_EQ(VN_BACKPRESSURE, harkentest::PlayRaw(&play, &voice));
    EXPECT_EQ(0u, voice);
}

TEST(VariableCurrentSlice, OneKeyServicesOnlyItsEarliestOverdueFrame) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);

    harken::VariableCallObservation first = harken::ObserveVariableCallAt(
        engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    first.requestedEffectiveFrame = 4 * kBlockFrames;
    harken::VariableCallObservation second = first;
    second.requestedEffectiveFrame = 5 * kBlockFrames;
    ASSERT_EQ(VN_OK, engine->variables.AcceptGlobal(
                         kVariable, 0.25f, first, engine->params,
                         engine->bindings, engine->busTargets)
                         .result);
    ASSERT_EQ(VN_OK, engine->variables.AcceptGlobal(
                         kVariable, 0.75f, second, engine->params,
                         engine->bindings, engine->busTargets)
                         .result);

    engine->nullBackend.Pump(6);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, engine->variables.AppliedCount());
    EXPECT_EQ(1u, engine->variables.PendingCount());
    float current = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(0.25f, current);

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(2u, engine->variables.AppliedCount());
    EXPECT_EQ(0u, engine->variables.PendingCount());
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(0.75f, current);
}

TEST(VariableCurrentSlice,
     GlobalAndEmitterSetHonorTicketOrderAfterServiceCursorRotation) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine, -24.0f, 0.0f, 0.0f, 0x47534554u);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const VnVoiceHandle voice =
        PlayDc(engine, harkentest::ObjectId(145), VN_BUS_MASTER, emitter);
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, render, harken::kTargetVoiceVolume,
        LinearCurve(-24.0f, -24.0f, 0.0f, 0.0f)));

    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    const harken::VariableCallObservation observation =
        harken::ObserveVariableCallAt(
            engine->variables.CurrentWorldGeneration(),
            engine->variables.CurrentTransportEpoch(), kBlockFrames,
            engine->timelinePool.WindowStartFrame(),
            engine->timelinePool.PublishCutoffFrame());
    const harken::VariableAcceptResult emitterWrite =
        engine->variables.AcceptEmitter(emitter, kVariable, -6.0f,
                                        observation, engine->emitters,
                                        engine->params);
    ASSERT_EQ(VN_OK, emitterWrite.result);
    const harken::VariableAcceptResult globalWrite =
        engine->variables.AcceptGlobal(kVariable, -12.0f, observation,
                                       engine->params, engine->bindings,
                                       engine->busTargets);
    ASSERT_EQ(VN_OK, globalWrite.result);
    ASSERT_LT(emitterWrite.ticket.sequence, globalWrite.ticket.sequence);

    engine->nullBackend.Pump(8);
    ASSERT_EQ(VN_OK, Vn_Update());
    const float globalGain = 0.5f * (1.0f + DbToLinear(-24.0f));
    bool heardLaterGlobal = false;
    bool haveFirstOutcome = false;
    harken::VariableOutcomeRecord firstOutcome{};
    for (std::uint32_t block = 0;
         block < 32 && engine->variables.PendingCount() != 0; ++block) {
        engine->nullBackend.Pump(1);
        const harken::VoiceRenderContext* voiceState =
            engine->voices.Resolve(render);
        ASSERT_NE(nullptr, voiceState);
        heardLaterGlobal =
            heardLaterGlobal ||
            std::abs(voiceState->gain.target - globalGain) < 1.0e-6f;
        ASSERT_EQ(VN_OK, Vn_Update());
        if (!haveFirstOutcome &&
            engine->variables.PopOutcome(&firstOutcome)) {
            haveFirstOutcome = true;
        }
    }
    ASSERT_EQ(0u, engine->variables.PendingCount());
    ASSERT_TRUE(haveFirstOutcome);
    EXPECT_TRUE(heardLaterGlobal);
    EXPECT_EQ(emitterWrite.ticket.sequence,
              firstOutcome.acceptedTicket.sequence);
    float global = 0.0f;
    float scoped = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &global));
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, kVariable, &scoped));
    EXPECT_FLOAT_EQ(-12.0f, global);
    EXPECT_FLOAT_EQ(-12.0f, scoped);
    EXPECT_FALSE(engine->emitters.LocalValue(emitter, kVariable, &scoped));
    ASSERT_NE(nullptr, engine->voices.Resolve(render));
    EXPECT_NEAR(globalGain, engine->voices.Resolve(render)->gain.target, 1.0e-6f);
}

TEST(VariableCurrentSlice,
     GlobalAndEmitterWritesSerializeByFrameAndTicketInBothOrders) {
    const auto runOrder = [](bool globalFirst, VnSoundId firstSound) {
        const float localGain = 0.75f + 0.25f * DbToLinear(-24.0f);
        const float globalGain = 0.5f * (1.0f + DbToLinear(-24.0f));
        const float clearedGain = globalFirst ? localGain : globalGain;
        const float retainedGain = globalGain;
        const float clearedDb = 20.0f * std::log10(clearedGain);
        const float retainedDb = 20.0f * std::log10(retainedGain);
        VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
        config.maxEmitters = 3;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        InstallVariable(engine, -24.0f, 0.0f, 0.0f,
                        globalFirst ? 0x47434F31 : 0x43474F31);

        VnEmitterHandle clearedEmitter = 0;
        VnEmitterHandle retainedEmitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&clearedEmitter));
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&retainedEmitter));
        const VnVoiceHandle clearedVoice =
            PlayDc(engine, firstSound, VN_BUS_MASTER, clearedEmitter);
        const VnVoiceHandle retainedVoice =
            PlayDc(engine, harkentest::OffsetObjectId(firstSound, 1), VN_BUS_MASTER, retainedEmitter);
        const VnVoiceHandle clearedRender =
            harkentest::RenderHandle(engine, clearedVoice);
        const VnVoiceHandle retainedRender =
            harkentest::RenderHandle(engine, retainedVoice);
        ASSERT_TRUE(engine->params.BindVoiceTarget(
            kVariable, clearedRender, harken::kTargetVoiceVolume,
            LinearCurve(-24.0f, -24.0f, 0.0f, 0.0f)));
        ASSERT_TRUE(engine->params.BindVoiceTarget(
            kVariable, retainedRender, harken::kTargetVoiceVolume,
            LinearCurve(-24.0f, -24.0f, 0.0f, 0.0f)));
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(2);

        ASSERT_EQ(VN_OK,
                  Vn_SetEmitterValue(clearedEmitter, kVariable, -6.0f));
        harkentest::AdvanceVariableWrite(engine);
        ASSERT_EQ(VN_OK,
                  Vn_SetEmitterValue(retainedEmitter, kVariable, -3.0f));
        harkentest::AdvanceVariableWrite(engine);

        harken::VariableCallObservation observation =
            harken::ObserveVariableCallAt(
                engine->variables.CurrentWorldGeneration(),
                engine->variables.CurrentTransportEpoch(), kBlockFrames,
                engine->timelinePool.WindowStartFrame(),
                engine->timelinePool.PublishCutoffFrame());
        const auto acceptGlobal = [&] {
            return engine->variables.AcceptGlobal(
                kVariable, -12.0f, observation, engine->params,
                engine->bindings, engine->busTargets);
        };
        const auto acceptLocal = [&] {
            return engine->variables.AcceptEmitter(
                clearedEmitter, kVariable, -6.0f, observation, engine->emitters,
                engine->params);
        };
        if (globalFirst) {
            ASSERT_EQ(VN_OK, acceptGlobal().result);
            ASSERT_EQ(VN_OK, acceptLocal().result);
        } else {
            ASSERT_EQ(VN_OK, acceptLocal().result);
            ASSERT_EQ(VN_OK, acceptGlobal().result);
        }

        const std::uint64_t appliedBefore = engine->variables.AppliedCount();
        for (std::uint32_t step = 0;
             step < 32 && engine->variables.AppliedCount() == appliedBefore;
             ++step) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
        }
        ASSERT_EQ(appliedBefore + 1, engine->variables.AppliedCount());
        ASSERT_EQ(1u, engine->variables.PendingCount());

        float global = 0.0f;
        float cleared = 0.0f;
        float retained = 0.0f;
        ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &global));
        ASSERT_TRUE(engine->variables.CurrentValue(clearedEmitter, kVariable,
                                                   &cleared));
        ASSERT_TRUE(engine->variables.CurrentValue(retainedEmitter, kVariable,
                                                   &retained));
        EXPECT_FLOAT_EQ(globalFirst ? -12.0f : 0.0f, global);
        EXPECT_FLOAT_EQ(globalFirst ? -12.0f : -6.0f, cleared);
        EXPECT_FLOAT_EQ(globalFirst ? -12.0f : -3.0f, retained);
        const float firstDb = 20.0f * std::log10(globalFirst ? globalGain : localGain);
        EXPECT_NEAR(firstDb,
                    engine->params.ResolvedValue(
                        clearedRender, harken::kTargetVoiceVolume), 1.0e-5f);
        EXPECT_NEAR(firstDb,
                    engine->params.LastCommitted(
                        clearedRender, harken::kTargetVoiceVolume), 1.0e-5f);

        harkentest::AdvanceVariableWrite(engine);
        ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &global));
        ASSERT_TRUE(engine->variables.CurrentValue(clearedEmitter, kVariable,
                                                   &cleared));
        ASSERT_TRUE(engine->variables.CurrentValue(retainedEmitter, kVariable,
                                                   &retained));
        EXPECT_FLOAT_EQ(-12.0f, global);
        EXPECT_FLOAT_EQ(globalFirst ? -6.0f : -12.0f, cleared);
        EXPECT_FLOAT_EQ(-12.0f, retained);
        EXPECT_EQ(globalFirst, engine->emitters.LocalValue(clearedEmitter, kVariable, &cleared));
        EXPECT_FALSE(engine->emitters.LocalValue(retainedEmitter, kVariable, &retained));
        EXPECT_NEAR(clearedDb, engine->params.ResolvedValue(
                                  clearedRender,
                                  harken::kTargetVoiceVolume), 1.0e-5f);
        EXPECT_NEAR(clearedDb, engine->params.LastCommitted(
                                  clearedRender,
                                  harken::kTargetVoiceVolume), 1.0e-5f);
        EXPECT_NEAR(retainedDb, engine->params.ResolvedValue(
                                   retainedRender,
                                   harken::kTargetVoiceVolume), 1.0e-5f);
        EXPECT_NEAR(retainedDb, engine->params.LastCommitted(
                                   retainedRender,
                                   harken::kTargetVoiceVolume), 1.0e-5f);
        EXPECT_FALSE(engine->params.PendingPublication(
            clearedRender, harken::kTargetVoiceVolume));
        EXPECT_FALSE(engine->params.PendingPublication(
            retainedRender, harken::kTargetVoiceVolume));
        EXPECT_FALSE(engine->params.ForeignClaim(
            clearedRender, harken::kTargetVoiceVolume));
        EXPECT_FALSE(engine->params.ForeignClaim(
            retainedRender, harken::kTargetVoiceVolume));
        ASSERT_NE(nullptr, engine->voices.Resolve(clearedRender));
        ASSERT_NE(nullptr, engine->voices.Resolve(retainedRender));
        EXPECT_NEAR(clearedGain,
                    engine->voices.Resolve(clearedRender)->gain.target,
                    1.0e-6f);
        EXPECT_NEAR(retainedGain,
                    engine->voices.Resolve(retainedRender)->gain.target,
                    1.0e-6f);
    };

    runOrder(true, harkentest::ObjectId(8101));
    runOrder(false, harkentest::ObjectId(8201));
}

TEST(VariableCurrentSlice,
     PersistentRoundRobinReachesThirtyThirdKeyUnderSlicePressure) {
    constexpr std::uint32_t kKeyCount =
        harken::kVariableWritesPerService + 1;
    constexpr VnVariableId kFirstVariable = harkentest::OffsetObjectId(kVariable, 100);
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = harken::kVariableWritesPerService;
    config.maxBindingAttachments = harken::kVariableWritesPerService;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    std::array<harken::VariableDefinitionV1, kKeyCount> definitions{};
    for (std::uint32_t i = 0; i < kKeyCount; ++i) {
        definitions[i] = MakeVariableDefinition(
            harkentest::OffsetObjectId(kFirstVariable, i), harken::VariableKindV1::Smooth,
            0.0f, 1.0f, 0.0f);
    }
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(
                  engine, definitions.data(), kKeyCount, 0x52523333)
                  .status);
    for (std::uint32_t i = 0; i < harken::kVariableWritesPerService; ++i) {
        RegisterLinearBinding(harkentest::ObjectId(9000 + i), harkentest::OffsetObjectId(kFirstVariable, i),
                              VN_TARGET_SCOPE_BUS,
                              harken::kBusTargetBusVolume, 0.0f, -6.0f);
        VnBindingAttachDesc attach{};
        attach.structSize = sizeof(attach);
        attach.scopeKind = VN_TARGET_SCOPE_BUS;
        attach.objectId = VN_BUS_MASTER;
        ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(9000 + i), &attach));
    }
    ASSERT_EQ(VN_OK, Vn_Update());

    std::vector<harken::VariableSliceRef> blockers;
    while (engine->variableSlices.FreeTargetCapacity() != 0) {
        const std::uint32_t count = std::min(
            engine->variableSlices.MaxTargetsPerSlice(),
            engine->variableSlices.FreeTargetCapacity());
        harken::VariableSliceRef ref{};
        ASSERT_TRUE(engine->variableSlices.Reserve(
            0x5252, engine->variables.CurrentTransportEpoch(),
            blockers.size() + 1, 0, blockers.size() + 1, count, &ref));
        blockers.push_back(ref);
    }

    harken::VariableCallObservation observation = harken::ObserveVariableCallAt(
        engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), kBlockFrames,
        engine->mixer.BlocksRendered() * kBlockFrames,
        engine->mixer.BlocksRendered() * kBlockFrames);
    observation.requestedEffectiveFrame =
        engine->mixer.BlocksRendered() * kBlockFrames;
    for (std::uint32_t i = 0; i < kKeyCount; ++i) {
        ASSERT_EQ(VN_OK,
                  engine->variables.AcceptGlobal(
                      harkentest::OffsetObjectId(kFirstVariable, i), 0.5f, observation, engine->params,
                      engine->bindings, engine->busTargets)
                      .result);
    }

    engine->variables.BeginService(*engine, engine->mixer.BlocksRendered(),
                                   kBlockFrames);
    engine->variables.EndService(*engine);
    EXPECT_EQ(kKeyCount, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->variables.AppliedCount());

    engine->nullBackend.Pump(1);
    engine->variables.BeginService(*engine, engine->mixer.BlocksRendered(),
                                   kBlockFrames);
    engine->variables.EndService(*engine);
    EXPECT_EQ(harken::kVariableWritesPerService,
              engine->variables.PendingCount());
    EXPECT_EQ(1u, engine->variables.AppliedCount());
    float current = -1.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(
        harkentest::OffsetObjectId(kFirstVariable, harken::kVariableWritesPerService), &current));
    EXPECT_FLOAT_EQ(0.5f, current);
    for (std::uint32_t i = 0; i < harken::kVariableWritesPerService; ++i) {
        ASSERT_TRUE(engine->variables.CurrentValue(harkentest::OffsetObjectId(kFirstVariable, i),
                                                  &current));
        EXPECT_FLOAT_EQ(0.0f, current);
    }
    for (harken::VariableSliceRef ref : blockers) {
        ASSERT_TRUE(engine->variableSlices.AbandonPrepared(ref));
    }
}

TEST(VariableCurrentSlice, PublicationRefusesPhysicalAndUnreclaimedSlicesWithoutDraining) {
    EngineSession session(MakeNullConfig(4, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    InstallVariable(engine);
    const auto voice = PlayDc(engine, harkentest::ObjectId(1), VN_BUS_MASTER);
    const auto render = harkentest::RenderHandle(engine, voice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(kVariable, render, harken::kTargetVoiceVolume,
                                             LinearCurve(0, 0, 1, -12)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.5f));
    const auto ticket = engine->logicCommits.IssuedCount();
    MakeAcceptedWriteDue(engine);
    const auto slice = engine->variables.SliceForTicketForTests(ticket);
    ASSERT_TRUE(slice.Valid());
    ASSERT_TRUE(engine->params.ForeignClaim(render, harken::kTargetVoiceVolume));
    const auto state = engine->variableSlices.State(slice);
    const auto freeTargets = engine->variableSlices.FreeTargetCapacity();
    const auto world = engine->variables.CurrentWorldGeneration();
    const auto definition = *engine->variables.Definition(kVariable);
    harken::VariableServiceV1::PreparedPackDefinitions token;
    const auto prepare = [&] {
        return engine->variables.PreparePackDefinitions(&definition, 1,
            harken::PackVariablePublicationReason::InitPublication, 123, &token);
    };
    EXPECT_EQ(VN_BACKPRESSURE, prepare());
    EXPECT_FALSE(token.IsPrepared());
    EXPECT_FALSE(engine->variables.DrainingWorld());
    EXPECT_EQ(state, engine->variableSlices.State(slice));
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
    EXPECT_EQ(1u, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->variables.TerminationCount());
    EXPECT_EQ(world, engine->variables.CurrentWorldGeneration());
    for (int i = 0; i < 32 && engine->variables.PendingCount() != 0; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    ASSERT_EQ(0u, engine->variables.PendingCount());
    ASSERT_TRUE(engine->variableSlices.HasUnreclaimedWorldRows(world));
    EXPECT_EQ(VN_BACKPRESSURE, prepare());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, prepare());
    engine->variables.CommitPreparedPackDefinitions(&token);
    EXPECT_EQ(world + 1, engine->variables.CurrentWorldGeneration());
}

TEST(VariableCurrentSlice, PublicationAcceptsNoPhysicalFrontierAndResetsBankGenerations) {
    EngineSession session(MakeNullConfig(4, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.5f));
    e.nullBackend.Pump(8);
    e.variables.BeginService(e, e.mixer.BlocksRendered(), kBlockFrames);
    ASSERT_TRUE(harken::detail::VariableServiceTestAccess::HasNoPhysicalFrontier(e.variables));
    harken::detail::VariableServiceTestAccess::SeedGlobalBankVersionGeneration(e.variables, UINT32_MAX);
    ASSERT_TRUE(harken::detail::VariableServiceTestAccess::SeedEmitterBankVersionGeneration(e.variables, emitter, UINT32_MAX));
    const auto definition = *e.variables.Definition(kVariable);
    harken::VariableServiceV1::PreparedPackDefinitions token;
    ASSERT_EQ(VN_OK, e.variables.PreparePackDefinitions(&definition, 1,
        harken::PackVariablePublicationReason::InitPublication, 777, &token));
    e.variables.CommitPreparedPackDefinitions(&token);
    EXPECT_EQ((std::array<std::uint32_t, 2>{1, 1}),
              harken::detail::VariableServiceTestAccess::BankGenerations(e.variables, emitter));
    harken::VariableOutcomeRecord record{};
    ASSERT_TRUE(e.variables.PopOutcome(&record));
    EXPECT_EQ(harken::VariableOutcomeKind::WorldReplacement, record.outcome);
    EXPECT_EQ(777u, record.actualFrame);
    e.variables.EndService(e);
    EXPECT_EQ(0u, e.variables.AppliedCount());
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.25f));
}

TEST(VariableCurrentSlice, PublicationRefusesPreparedRowsInvalidHistoryAndExhaustedWorld) {
    EngineSession session(MakeNullConfig(4, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.5f));
    const auto world = e.variables.CurrentWorldGeneration();
    const auto definition = *e.variables.Definition(kVariable);
    harken::VariableServiceV1::PreparedPackDefinitions token;
    const auto prepare = [&] {
        return e.variables.PreparePackDefinitions(&definition, 1,
            harken::PackVariablePublicationReason::InitPublication, 0, &token);
    };
    harken::PreparedVariableWriteToken write;
    const auto observation = harken::ObserveVariableCallAt(world, e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.75f, observation, 0, &write).result);
    EXPECT_EQ(VN_BACKPRESSURE, prepare());
    e.variables.CancelPreparedWrite(&write);
    for (int variant = 0; variant < 8; ++variant) {
        SCOPED_TRACE(variant);
        harken::detail::VariableServiceTestAccess::SeedHistoryEligibility(e.variables, variant);
        EXPECT_EQ(VN_BACKPRESSURE, prepare());
        EXPECT_FALSE(token.IsPrepared());
        EXPECT_EQ(1u, e.variables.PendingCount());
        EXPECT_EQ(0u, e.variables.TerminationCount());
    }
    harken::detail::VariableServiceTestAccess::SeedHistoryEligibility(e.variables, -1);
    harken::detail::VariableServiceTestAccess::SeedWorld(e.variables, UINT64_MAX);
    EXPECT_EQ(VN_RUNTIME_RECREATE_REQUIRED, prepare());
    EXPECT_EQ(UINT64_MAX, e.variables.CurrentWorldGeneration());
    harken::detail::VariableServiceTestAccess::SeedWorld(e.variables, world);
    ASSERT_EQ(VN_OK, prepare());
    EXPECT_EQ(1u, e.variables.PendingCount());
}

TEST(VariableCurrentSlice, PublicationOutcomeTailFitsFullHistoryWithoutEarlySlotReuse) {
    using Access = harken::detail::VariableServiceTestAccess;
    EngineSession session(MakeNullConfig(4, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const auto firstTicket = e.logicCommits.IssuedCount() + 1;
    const void* storage = Access::OutcomeStorage(e.variables);
    constexpr std::size_t capacity = harken::kVariableHistoryCapacity;
    std::array<bool, capacity> seen{};
    harken::VariableOutcomeRecord record{};
    for (std::size_t i = 0; i < capacity - 1; ++i) {
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, i % 2 ? 0.25f : 0.5f));
        if (i == capacity / 2) {
            std::size_t delivered = 0;
            while (e.variables.PopOutcome(&record)) {
                const auto index = record.acceptedTicket.sequence - firstTicket;
                ASSERT_LT(index, capacity);
                EXPECT_FALSE(seen[index]);
                seen[index] = true;
                ++delivered;
            }
            ASSERT_EQ(capacity / 2, delivered);
            e.variables.MarkOutcomeDeliveryComplete();
        }
    }
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, 0.75f));
    ASSERT_EQ((std::array<std::size_t, 3>{capacity - 2, capacity, 0}), Access::OutcomeCapacity(e.variables));
    const auto definition = *e.variables.Definition(kVariable);
    const auto world = e.variables.CurrentWorldGeneration();
    harken::VariableServiceV1::PreparedPackDefinitions token;
    ASSERT_EQ(VN_OK, e.variables.PreparePackDefinitions(&definition, 1,
        harken::PackVariablePublicationReason::InitPublication, 987, &token));
    e.variables.CommitPreparedPackDefinitions(&token);
    EXPECT_EQ(storage, Access::OutcomeStorage(e.variables));
    EXPECT_EQ((std::array<std::size_t, 3>{capacity, capacity, 0}), Access::OutcomeCapacity(e.variables));
    EXPECT_EQ(2u, e.variables.TerminationCount());
    EXPECT_EQ(VN_VARIABLE_TRANSACTION_EXHAUSTED, Vn_SetValue(kVariable, 0.25f));
    e.variables.FinishOutcomeDelivery();
    EXPECT_EQ((std::array<std::size_t, 3>{capacity / 2, capacity, capacity / 2}), Access::OutcomeCapacity(e.variables));
    std::size_t terminations = 0;
    while (e.variables.PopOutcome(&record)) {
        const auto index = record.acceptedTicket.sequence - firstTicket;
        ASSERT_LT(index, capacity);
        EXPECT_FALSE(seen[index]);
        seen[index] = true;
        if (record.outcome == harken::VariableOutcomeKind::WorldReplacement) {
            EXPECT_EQ(world, record.world);
            EXPECT_EQ(987u, record.actualFrame);
            ++terminations;
        }
    }
    EXPECT_EQ(2u, terminations);
    EXPECT_TRUE(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.25f));
    EXPECT_EQ(storage, Access::OutcomeStorage(e.variables));
}

TEST(VariableCurrentSlice, WorldReplacementDrainsPhysicalCutAndClosesPlay) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine, -24.0f, 0.0f, 0.0f, 0x5731);
    const VnVoiceHandle voice = PlayDc(engine, harkentest::ObjectId(1), VN_BUS_MASTER);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, harkentest::RenderHandle(engine, voice),
        harken::kTargetVoiceVolume,
        LinearCurve(-24.0f, -24.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, -24.0f));
    MakeAcceptedWriteDue(engine);
    CurrentSliceScratchReset scratchReset{engine};
    engine->mixer.SetCurrentSliceScratchLimitForTest(0);
    const harken::VariableDefinitionV1 replacement = MakeVariableDefinition(
        kVariable, harken::VariableKindV1::Smooth, -48.0f, 6.0f, 0.0f);
    const harken::WorldInstallResult blocked = harkentest::InstallDefinitionsWorld(
        engine, &replacement, 1, 0x5732);
    ASSERT_EQ(harken::WorldInstallStatus::Backpressure, blocked.status);
    const std::uint64_t oldGeneration =
        engine->variables.CurrentWorldGeneration();

    static const std::vector<float> samples(kSampleRate, 0.05f);
    const VnPCMDesc pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &pcm));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(2));
    VnVoiceHandle duringDrain = 0;
    EXPECT_EQ(VN_BACKPRESSURE, harkentest::PlayRaw(&play, &duringDrain));
    EXPECT_EQ(0u, duringDrain);
    EXPECT_EQ(oldGeneration, engine->variables.CurrentWorldGeneration());

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::WorldInstallStatus::Backpressure,
              harkentest::InstallDefinitionsWorld(engine, &replacement, 1,
                                                  0x5732)
                  .status);
    EXPECT_EQ(oldGeneration, engine->variables.CurrentWorldGeneration());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::WorldInstallResult stillBlocked =
        harkentest::InstallDefinitionsWorld(engine, &replacement, 1, 0x5732);
    EXPECT_EQ(harken::WorldInstallStatus::Backpressure, stillBlocked.status);
    EXPECT_EQ(oldGeneration, engine->variables.CurrentWorldGeneration());
    EXPECT_EQ(1u, engine->variables.PendingCount());

    engine->mixer.RestoreCurrentSliceScratchLimitForTest();
    for (std::uint32_t block = 0;
         block < engine->timelinePool.PageCount() &&
         engine->variables.PendingCount() != 0;
         ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    ASSERT_EQ(0u, engine->variables.PendingCount());
    const harken::WorldInstallResult equalityBlocked =
        harkentest::InstallDefinitionsWorld(engine, &replacement, 1, 0x5732);
    ASSERT_EQ(harken::WorldInstallStatus::Backpressure,
              equalityBlocked.status);
    EXPECT_EQ(oldGeneration, engine->variables.CurrentWorldGeneration());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::WorldInstallResult installed =
        harkentest::InstallDefinitionsWorld(engine, &replacement, 1, 0x5732);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, installed.status);
    EXPECT_GT(installed.resultGeneration, oldGeneration);

    VnVoiceHandle afterDrain = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &afterDrain));
    EXPECT_NE(0u, afterDrain);
}

TEST(VariableCurrentSlice, ResetUsesEngineEpochAndPreservesAppliedBankBase) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine, -24.0f, 6.0f, 0.0f);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, -12.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, -6.0f));
    harkentest::AdvanceVariableWrite(engine);

    harken::VariableCallObservation future = harken::ObserveVariableCallAt(
        engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), kBlockFrames,
        engine->timelinePool.WindowStartFrame(),
        engine->timelinePool.PublishCutoffFrame());
    future.requestedEffectiveFrame =
        (engine->mixer.BlocksRendered() + 2) * kBlockFrames;
    ASSERT_EQ(VN_OK, engine->variables.AcceptGlobal(
                         kVariable, -3.0f, future, engine->params,
                         engine->bindings, engine->busTargets)
                         .result);
    const std::array<VnVariableId, 1> variables = {kVariable};
    std::array<float, 1> bankValues{};
    harken::BankVersionToken globalBefore{};
    harken::BankVersionToken emitterBefore{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        0, variables.data(), variables.size(), bankValues.data(),
        &globalBefore));
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        emitter, variables.data(), variables.size(), bankValues.data(),
        &emitterBefore));
    const std::uint64_t epochBefore =
        engine->variables.CurrentTransportEpoch();
    const std::uint64_t resetFrame =
        engine->mixer.BlocksRendered() * kBlockFrames;
    ASSERT_EQ(harken::RuntimeTransportResetStatus::Complete,
              harken::BeginRuntimeTransportReset(*engine, resetFrame));

    EXPECT_EQ(epochBefore + 1u,
              engine->variables.CurrentTransportEpoch());
    EXPECT_EQ(engine->timelinePool.Epoch(),
              engine->variables.CurrentTransportEpoch());
    harken::BankVersionToken globalAfter{};
    harken::BankVersionToken emitterAfter{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        0, variables.data(), variables.size(), bankValues.data(),
        &globalAfter));
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        emitter, variables.data(), variables.size(), bankValues.data(),
        &emitterAfter));
    EXPECT_EQ(globalBefore.slot, globalAfter.slot);
    EXPECT_EQ(globalBefore.generation + 1u, globalAfter.generation);
    EXPECT_EQ(emitterBefore.slot, emitterAfter.slot);
    EXPECT_EQ(emitterBefore.generation + 1u, emitterAfter.generation);
    float global = 0.0f;
    float local = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &global));
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, kVariable, &local));
    EXPECT_FLOAT_EQ(-12.0f, global);
    EXPECT_FLOAT_EQ(-6.0f, local);
    engine->nullBackend.Pump(3);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &global));
    EXPECT_FLOAT_EQ(-12.0f, global);
}

TEST(VariableCurrentSlice,
     ResetGeometryFailureDoesNotRaiseTheTransportEpoch) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);

    const std::uint64_t epochBefore =
        engine->variables.CurrentTransportEpoch();
    const std::uint64_t windowBefore =
        engine->timelinePool.WindowStartFrame();
    const std::uint64_t quantum = engine->timelinePool.QuantumFrames();
    const std::uint64_t resetFrame = (UINT64_MAX / quantum) * quantum;

    EXPECT_FALSE(engine->variables.ResetTransport(resetFrame));
    EXPECT_EQ(epochBefore, engine->variables.CurrentTransportEpoch());
    EXPECT_EQ(epochBefore, engine->timelinePool.Epoch());
    EXPECT_TRUE(engine->timelinePool.Valid());
    EXPECT_EQ(windowBefore, engine->timelinePool.WindowStartFrame());
}

TEST(VariableCurrentSlice,
     TransportResetWaitsStrictlyPastThePhysicalSliceFence) {
    VnConfig config = MakeNullConfig(3, kBlockFrames, kSampleRate);
    config.maxBindings = 2;
    config.maxBindingAttachments = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    RegisterBusVolumeBinding(harkentest::ObjectId(1), VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    static const std::vector<float> samples(kSampleRate, 0.05f);
    const VnPCMDesc pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(8301), &pcm));
    const std::uint32_t freeRows = engine->variableSlices.FreeRowCount();
    const std::uint32_t freeTargets =
        engine->variableSlices.FreeTargetCapacity();
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    for (std::uint32_t step = 0;
         step < 32 && engine->variables.PendingCount() != 0; ++step) {
        ASSERT_EQ(VN_OK, Vn_Update());
        if (engine->variables.PendingCount() == 0) {
            break;
        }
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(0u, engine->variables.PendingCount());
    ASSERT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    ASSERT_EQ(freeTargets - 1u,
              engine->variableSlices.FreeTargetCapacity());

    const std::uint64_t oldEpoch =
        engine->variables.CurrentTransportEpoch();
    const std::uint64_t oldWorld =
        engine->variables.CurrentWorldGeneration();
    ASSERT_TRUE(
        engine->variableSlices.HasUnreclaimedTransportRows(oldEpoch));
    const std::uint64_t resetFrame =
        engine->mixer.BlocksRendered() * kBlockFrames;
    EXPECT_EQ(harken::RuntimeTransportResetStatus::Backpressure,
              harken::BeginRuntimeTransportReset(*engine, resetFrame));
    EXPECT_EQ(oldEpoch, engine->variables.CurrentTransportEpoch());
    EXPECT_TRUE(engine->variables.DrainingTransport());
    EXPECT_FALSE(engine->variables.DrainingWorld());
    EXPECT_TRUE(engine->variables.AdmissionClosed());
    EXPECT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets - 1u,
              engine->variableSlices.FreeTargetCapacity());

    const std::uint64_t acceptedBefore = engine->variables.AcceptedCount();
    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetValue(kVariable, 0.5f));
    EXPECT_EQ(acceptedBefore, engine->variables.AcceptedCount());
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(8301));
    VnVoiceHandle refusedVoice = 0;
    EXPECT_EQ(VN_BACKPRESSURE, harkentest::PlayRaw(&play, &refusedVoice));
    EXPECT_EQ(0u, refusedVoice);
    const harken::VariableDefinitionV1 replacement = MakeVariableDefinition(
        kVariable, harken::VariableKindV1::Smooth, -1.0f, 1.0f, 0.0f);
    const harken::WorldInstallResult blocked =
        harkentest::InstallDefinitionsWorld(engine, &replacement, 1,
                                            0x54525232);
    EXPECT_EQ(harken::WorldInstallStatus::Backpressure, blocked.status);
    EXPECT_EQ(oldWorld, engine->variables.CurrentWorldGeneration());
    EXPECT_TRUE(engine->variables.DrainingTransport());
    EXPECT_FALSE(engine->variables.DrainingWorld());

    EXPECT_EQ(harken::RuntimeTransportResetStatus::InvalidRequest,
              harken::BeginRuntimeTransportReset(
                  *engine, resetFrame + kBlockFrames));
    EXPECT_EQ(oldEpoch, engine->variables.CurrentTransportEpoch());
    EXPECT_FALSE(engine->runtimeRecreateRequired);

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FALSE(
        engine->variableSlices.HasUnreclaimedTransportRows(oldEpoch));
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
    ASSERT_EQ(harken::RuntimeTransportResetStatus::Complete,
              harken::BeginRuntimeTransportReset(*engine, resetFrame));
    EXPECT_EQ(oldEpoch + 1u, engine->variables.CurrentTransportEpoch());
    EXPECT_EQ(engine->variables.CurrentTransportEpoch(),
              engine->timelinePool.Epoch());
    EXPECT_EQ(resetFrame - resetFrame % kBlockFrames,
              engine->timelinePool.WindowStartFrame());
    EXPECT_FALSE(engine->variables.AdmissionClosed());
    EXPECT_FALSE(engine->variables.DrainingTransport());
    float current = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(1.0f, current);

    VnVoiceHandle admittedVoice = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &admittedVoice));
    EXPECT_NE(0u, admittedVoice);
    EXPECT_EQ(VN_OK, Vn_SetValue(kVariable, 0.5f));
    harkentest::AdvanceVariableWrite(engine);
}

TEST(VariableCurrentSlice,
     VariableServiceDrainClosesEveryCurrentPlayableStartBeforeMutation) {
    VnConfig config = MakeNullConfig(3, kBlockFrames, kSampleRate);
    config.maxBindings = 2;
    config.maxBindingAttachments = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);

    static const std::vector<float> samples(kSampleRate, 0.05f);
    const VnPCMDesc pcm = MonoDesc(samples, kSampleRate);
    constexpr VnSoundId sound = harkentest::ObjectId(8302);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    const harken::VariableDefinitionV1 definition = MakeVariableDefinition(
        kVariable, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f);
    harken::PlayablePropertyProgramViewV1 property{};
    property.contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Sound;
    property.stableId = sound;
    property.authoredStableOrder = 0;
    property.contentRevision =
        engine->registry.Lookup(sound).contentRevision;
    property.routeStableId = VN_BUS_MASTER;
    property.compatibilityFingerprint = 1;
    property.payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    property.payload.values[3].bits = 1;
    property.payload.values[6].objectId = VN_BUS_MASTER;
    property.payload.values[11].bits =
        std::bit_cast<std::uint32_t>(1.0f);
    property.programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(property);
    harken::ContainerWorldInstallViewV1 install{};
    install.variables = &definition;
    install.variableCount = 1;
    install.playableProperties = &property;
    install.playablePropertyCount = 1;
    install.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties;
    install.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(install);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harken::InstallContainerWorldV1(*engine, install).status);
    RegisterBusVolumeBinding(harkentest::ObjectId(1), VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    for (std::uint32_t step = 0;
         step < 32 && engine->variables.PendingCount() != 0; ++step) {
        ASSERT_EQ(VN_OK, Vn_Update());
        if (engine->variables.PendingCount() != 0) {
            engine->nullBackend.Pump(1);
        }
    }
    ASSERT_EQ(0u, engine->variables.PendingCount());
    const std::uint64_t resetFrame =
        engine->mixer.BlocksRendered() * kBlockFrames;
    ASSERT_FALSE(engine->variables.ResetTransport(resetFrame));
    ASSERT_TRUE(engine->variables.DrainingTransport());

    harken::PlayableStartRequest request{};
    request.root.kind = harken::PlayableKindV1::Sound;
    request.root.stableId = sound;
    request.root.expectedProgramSemanticFingerprint =
        property.programSemanticFingerprint;
    request.root.contentRevision = property.contentRevision;
    request.root.routeStableId = property.routeStableId;
    request.root.compatibilityFingerprint =
        property.compatibilityFingerprint;
    const std::uint32_t instanceBaseline =
        engine->playableInstances.LiveCount();
    const std::uint32_t voiceBaseline = engine->voices.ActiveCount();
    const harken::PlayableStartResult current =
        harken::StartPlayable(*engine, request);
    EXPECT_EQ(harken::PlayableStartStatus::Backpressure, current.status);
    EXPECT_EQ(0u, current.instance);
    EXPECT_EQ(0u, current.ticket);
    const harken::PlayableWorldViewV1 currentWorld =
        engine->variables.CurrentPlayableWorld();
    const harken::PlayableStartResult explicitCurrent =
        harken::StartPlayable(*engine, currentWorld, request);
    EXPECT_EQ(harken::PlayableStartStatus::Backpressure,
              explicitCurrent.status);
    EXPECT_EQ(0u, explicitCurrent.instance);
    EXPECT_EQ(0u, explicitCurrent.ticket);
    EXPECT_EQ(instanceBaseline, engine->playableInstances.LiveCount());
    EXPECT_EQ(voiceBaseline, engine->voices.ActiveCount());

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_TRUE(engine->variables.ResetTransport(resetFrame));
}

TEST(VariableCurrentSlice,
     TransportResetEmitterBankGenerationExhaustionHasZeroPartialUpdate) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);
    InstallVariable(engine, -24.0f, 6.0f, -3.0f);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, -6.0f));
    harkentest::AdvanceVariableWrite(engine);

    ASSERT_TRUE(harken::detail::VariableServiceTestAccess::
                    SeedEmitterBankVersionGeneration(
                        engine->variables, emitter, UINT32_MAX));
    const std::array<VnVariableId, 1> variables = {kVariable};
    std::array<float, 1> values{};
    harken::BankVersionToken globalBefore{};
    harken::BankVersionToken emitterBefore{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        0, variables.data(), variables.size(), values.data(),
        &globalBefore));
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        emitter, variables.data(), variables.size(), values.data(),
        &emitterBefore));
    ASSERT_EQ(UINT32_MAX, emitterBefore.generation);
    const std::uint64_t epochBefore =
        engine->variables.CurrentTransportEpoch();
    const std::uint64_t windowBefore =
        engine->timelinePool.WindowStartFrame();
    const std::uint64_t resetFrame =
        engine->mixer.BlocksRendered() * kBlockFrames;

    EXPECT_EQ(harken::RuntimeTransportResetStatus::RuntimeRecreateRequired,
              harken::BeginRuntimeTransportReset(*engine, resetFrame));
    EXPECT_FALSE(engine->runtimeTransportResetActive);
    EXPECT_TRUE(engine->runtimeRecreateRequired);
    EXPECT_EQ(epochBefore, engine->variables.CurrentTransportEpoch());
    EXPECT_EQ(epochBefore, engine->timelinePool.Epoch());
    EXPECT_EQ(windowBefore, engine->timelinePool.WindowStartFrame());
    harken::BankVersionToken globalAfter{};
    harken::BankVersionToken emitterAfter{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        0, variables.data(), variables.size(), values.data(),
        &globalAfter));
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        emitter, variables.data(), variables.size(), values.data(),
        &emitterAfter));
    EXPECT_EQ(globalBefore.slot, globalAfter.slot);
    EXPECT_EQ(globalBefore.generation, globalAfter.generation);
    EXPECT_EQ(emitterBefore.slot, emitterAfter.slot);
    EXPECT_EQ(emitterBefore.generation, emitterAfter.generation);
}

TEST(VariableCurrentSlice,
     GlobalSetBankGenerationExhaustionHasZeroPartialUpdate) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);
    InstallVariable(engine, -24.0f, 6.0f, -3.0f);
    harken::detail::VariableServiceTestAccess::
        SeedGlobalBankVersionGeneration(engine->variables, UINT32_MAX);
    const std::array<VnVariableId, 1> variables = {kVariable};
    std::array<float, 1> values{};
    harken::BankVersionToken bankBefore{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        0, variables.data(), variables.size(), values.data(), &bankBefore));
    const float valueBefore = values[0];
    const std::uint64_t acceptedBefore = engine->variables.AcceptedCount();
    const std::uint64_t pendingBefore = engine->variables.PendingCount();
    const std::uint64_t historyBefore = engine->variables.HistoryHighWater();
    const std::uint64_t issuedBefore = engine->logicCommits.IssuedCount();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetValue(kVariable, -6.0f));
    EXPECT_EQ(acceptedBefore, engine->variables.AcceptedCount());
    EXPECT_EQ(pendingBefore, engine->variables.PendingCount());
    EXPECT_EQ(historyBefore, engine->variables.HistoryHighWater());
    EXPECT_EQ(issuedBefore, engine->logicCommits.IssuedCount());
    harken::BankVersionToken bankAfter{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        0, variables.data(), variables.size(), values.data(), &bankAfter));
    EXPECT_EQ(bankBefore.slot, bankAfter.slot);
    EXPECT_EQ(bankBefore.generation, bankAfter.generation);
    EXPECT_FLOAT_EQ(valueBefore, values[0]);
}

TEST(VariableCurrentSlice,
     EmitterSetBankGenerationExhaustionHasZeroPartialUpdate) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);
    InstallVariable(engine, -24.0f, 6.0f, -3.0f);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kVariable, -6.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_TRUE(harken::detail::VariableServiceTestAccess::
                    SeedEmitterBankVersionGeneration(
                        engine->variables, emitter, UINT32_MAX));
    const std::array<VnVariableId, 1> variables = {kVariable};
    std::array<float, 1> values{};
    harken::BankVersionToken bankBefore{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        emitter, variables.data(), variables.size(), values.data(),
        &bankBefore));
    const float valueBefore = values[0];
    const std::uint64_t acceptedBefore = engine->variables.AcceptedCount();
    const std::uint64_t pendingBefore = engine->variables.PendingCount();
    const std::uint64_t historyBefore = engine->variables.HistoryHighWater();
    const std::uint64_t issuedBefore = engine->logicCommits.IssuedCount();

    EXPECT_EQ(VN_BACKPRESSURE,
              Vn_SetEmitterValue(emitter, kVariable, -12.0f));
    EXPECT_EQ(acceptedBefore, engine->variables.AcceptedCount());
    EXPECT_EQ(pendingBefore, engine->variables.PendingCount());
    EXPECT_EQ(historyBefore, engine->variables.HistoryHighWater());
    EXPECT_EQ(issuedBefore, engine->logicCommits.IssuedCount());
    harken::BankVersionToken bankAfter{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        emitter, variables.data(), variables.size(), values.data(),
        &bankAfter));
    EXPECT_EQ(bankBefore.slot, bankAfter.slot);
    EXPECT_EQ(bankBefore.generation, bankAfter.generation);
    EXPECT_FLOAT_EQ(valueBefore, values[0]);
}

TEST(VariableCurrentSlice,
     GlobalSetRejectsBeforeAnyLiveEmitterBankGenerationCanWrap) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 3;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);
    InstallVariable(engine, -24.0f, 6.0f, -3.0f);
    VnEmitterHandle first = 0;
    VnEmitterHandle second = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&first));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&second));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(first, kVariable, -6.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(second, kVariable, -9.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_TRUE(harken::detail::VariableServiceTestAccess::
                    SeedEmitterBankVersionGeneration(
                        engine->variables, second, UINT32_MAX));
    const std::array<VnVariableId, 1> variables = {kVariable};
    std::array<float, 1> values{};
    harken::BankVersionToken globalBefore{};
    harken::BankVersionToken firstBefore{};
    harken::BankVersionToken secondBefore{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        0, variables.data(), variables.size(), values.data(),
        &globalBefore));
    const float globalValueBefore = values[0];
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        first, variables.data(), variables.size(), values.data(),
        &firstBefore));
    const float firstValueBefore = values[0];
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        second, variables.data(), variables.size(), values.data(),
        &secondBefore));
    const float secondValueBefore = values[0];
    const std::uint64_t acceptedBefore = engine->variables.AcceptedCount();
    const std::uint64_t pendingBefore = engine->variables.PendingCount();
    const std::uint64_t issuedBefore = engine->logicCommits.IssuedCount();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetValue(kVariable, -12.0f));
    EXPECT_EQ(acceptedBefore, engine->variables.AcceptedCount());
    EXPECT_EQ(pendingBefore, engine->variables.PendingCount());
    EXPECT_EQ(issuedBefore, engine->logicCommits.IssuedCount());
    harken::BankVersionToken globalAfter{};
    harken::BankVersionToken firstAfter{};
    harken::BankVersionToken secondAfter{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        0, variables.data(), variables.size(), values.data(),
        &globalAfter));
    EXPECT_EQ(globalBefore.generation, globalAfter.generation);
    EXPECT_FLOAT_EQ(globalValueBefore, values[0]);
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        first, variables.data(), variables.size(), values.data(),
        &firstAfter));
    EXPECT_EQ(firstBefore.generation, firstAfter.generation);
    EXPECT_FLOAT_EQ(firstValueBefore, values[0]);
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        second, variables.data(), variables.size(), values.data(),
        &secondAfter));
    EXPECT_EQ(secondBefore.generation, secondAfter.generation);
    EXPECT_FLOAT_EQ(secondValueBefore, values[0]);
}

TEST(VariableCurrentSlice,
     RetiringVoiceTargetsRemainOwnedAcrossRenderSlotGenerationReuse) {
    harken::EngineConfig config{};
    config.maxVariableSliceTargetsTotal = 2;
    harken::ParamSystem params(config);
    constexpr VnVoiceHandle kOldHandle =
        (static_cast<VnVoiceHandle>(7) << 32);
    constexpr VnVoiceHandle kNewHandle =
        (static_cast<VnVoiceHandle>(8) << 32);
    params.OnVoiceStarted(kOldHandle, -6.0f, 1.0);
    (void)params.SetApiDirect(kOldHandle, harken::kTargetVoiceVolume, -6.0f);
    (void)params.SetApiDirect(kOldHandle, harken::kTargetVoiceLowpassCutoff,
                              12000.0f);
    const harken::VariableSliceRef volumeOwner{0, 1};
    const harken::VariableSliceRef filterOwner{1, 1};
    ASSERT_TRUE(params.TryClaim(
        kOldHandle, harken::kTargetVoiceVolume, volumeOwner,
        params.TargetRevision(kOldHandle, harken::kTargetVoiceVolume)));
    ASSERT_TRUE(params.TryClaim(
        kOldHandle, harken::kTargetVoiceLowpassCutoff, filterOwner,
        params.TargetRevision(kOldHandle,
                              harken::kTargetVoiceLowpassCutoff)));

    params.RetireVoiceTargets(kOldHandle);
    ASSERT_EQ(2u, params.RetiringTargetCountForTests());
    params.OnVoiceStarted(kNewHandle, 0.0f, 1.0);
    const harken::VariableSliceRef newOwner{2, 1};
    ASSERT_TRUE(params.TryClaim(
        kNewHandle, harken::kTargetVoiceVolume, newOwner,
        params.TargetRevision(kNewHandle, harken::kTargetVoiceVolume)));

    float resolved = 0.0f;
    std::uint32_t rampMs = 0;
    std::uint32_t revision = 0;
    ASSERT_TRUE(params.SnapshotForClaim(
        kOldHandle, harken::kTargetVoiceVolume, volumeOwner, &resolved,
        &rampMs, &revision));
    EXPECT_FLOAT_EQ(-12.0f, resolved);
    ASSERT_TRUE(params.SnapshotForClaim(
        kOldHandle, harken::kTargetVoiceLowpassCutoff, filterOwner, &resolved,
        &rampMs, &revision));
    EXPECT_FLOAT_EQ(12000.0f, resolved);

    params.ReleaseClaim(kOldHandle, harken::kTargetVoiceVolume, volumeOwner);
    EXPECT_EQ(1u, params.RetiringTargetCountForTests());
    params.MarkSliceCommitted(kOldHandle, harken::kTargetVoiceLowpassCutoff,
                              filterOwner, 12000.0f, 0);
    EXPECT_EQ(0u, params.RetiringTargetCountForTests());
    EXPECT_TRUE(params.ForeignClaim(kNewHandle,
                                    harken::kTargetVoiceVolume));
    params.ReleaseClaim(kNewHandle, harken::kTargetVoiceVolume, newOwner);
}

VnVariableTicketV1 AcceptGlobalWrite(VnVariableId variable, float value) {
    VnVariableWriteV1 write{};
    write.variableId = variable;
    write.value = value;
    VnVariableBatchDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.writeCount = 1;
    desc.writes = &write;
    VnVariableTicketV1 ticket{};
    EXPECT_EQ(VN_OK, Vn_SetValues(&desc, &ticket));
    return ticket;
}

VnVariableTicketQueryResultV1 QueryTicket(const VnVariableTicketV1& ticket) {
    VnVariableTicketQueryDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.ticket = ticket;
    VnVariableTicketQueryResultV1 out{};
    out.structSize = sizeof(out);
    EXPECT_EQ(VN_OK, Vn_QueryVariableTicket(&desc, &out));
    return out;
}

float QueryCurrentValue(VnVariableId variable) {
    VnVariableQueryDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = variable;
    VnVariableQueryResultV1 out{};
    out.structSize = sizeof(out);
    EXPECT_EQ(VN_OK, Vn_QueryVariable(&desc, &out));
    return out.currentValue;
}

TEST(VariableCurrentSlice, FiniteOverflowWriteRowIsTargetRejected) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxBuses = 4;
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(9), VN_BUS_MASTER));
    const VnVoiceHandle voice = PlayDc(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, render, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, 772.0f)));
    RegisterBusVolumeBinding(harkentest::ObjectId(1), harkentest::ObjectId(9), 0.0f, 0.0f, 1.0f, -12.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    harken::VoiceRenderContext* voiceState = engine->voices.Resolve(render);
    harken::BusTree::Bus* busState =
        engine->buses.BusState(engine->buses.FindIndex(harkentest::ObjectId(9)));
    ASSERT_NE(nullptr, voiceState);
    ASSERT_NE(nullptr, busState);
    ASSERT_FLOAT_EQ(1.0f, voiceState->gain.target);
    ASSERT_FLOAT_EQ(1.0f, busState->gain.target);
    const std::uint64_t invalidBefore =
        engine->voices.InvalidVolumeApplyCount();
    const std::uint64_t commandHead = engine->commands.PublishedHead();

    const VnVariableTicketV1 rejectedTicket = AcceptGlobalWrite(kVariable, 1.0f);
    ASSERT_NE(0u, rejectedTicket.sequence);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    const auto rejected = QueryTicket(rejectedTicket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, rejected.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_REJECTED_TARGET, rejected.outcome);
    EXPECT_FLOAT_EQ(0.0f, QueryCurrentValue(kVariable));
    EXPECT_FLOAT_EQ(1.0f, voiceState->gain.target);
    EXPECT_FLOAT_EQ(1.0f, busState->gain.target);
    EXPECT_EQ(invalidBefore, engine->voices.InvalidVolumeApplyCount());
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_FALSE(engine->params.ForeignClaim(render,
                                             harken::kTargetVoiceVolume));
    EXPECT_FALSE(engine->busTargets.ForeignClaim(
        engine->buses.FindIndex(harkentest::ObjectId(9)), harken::kBusTargetBusVolume));

    const VnVariableTicketV1 legalTicket = AcceptGlobalWrite(kVariable, 0.1f);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    const auto applied = QueryTicket(legalTicket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, applied.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_APPLIED, applied.outcome);
    EXPECT_FLOAT_EQ(0.1f, QueryCurrentValue(kVariable));
    EXPECT_FLOAT_EQ(1.0f / 0.9f, voiceState->gain.target);
    EXPECT_FLOAT_EQ(0.9f + 0.1f * DbToLinear(-12.0f), busState->gain.target);
}

TEST(VariableCurrentSlice, RejectedGlobalBatchRestoresAllEmitterOverridesAndProjections) {
    EngineSession session(MakeNullConfig(4, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto otherVariable = harkentest::ObjectId(7002);
    const harken::VariableDefinitionV1 definitions[] = {
        MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0, 1, 0),
        MakeVariableDefinition(otherVariable, harken::VariableKindV1::Smooth, 0, 1, 0)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, definitions, 2).status);
    VnEmitterHandle a = 0;
    VnEmitterHandle b = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&a));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&b));
    const auto voice = PlayDc(engine, harkentest::ObjectId(1), VN_BUS_MASTER, a);
    const auto render = harkentest::RenderHandle(engine, voice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kVariable, render, harken::kTargetVoiceVolume,
        LinearCurve(0, 0, 1, 772)));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(a, kVariable, 0.25f));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(b, kVariable, 0.5f));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(a, otherVariable, 0.75f));
    harkentest::AdvanceVariableWrite(engine);
    const float oldGain = engine->voices.Resolve(render)->gain.target;
    const float oldResolved = engine->params.ResolvedValue(render, harken::kTargetVoiceVolume);
    VnVariableWriteV1 writes[2]{};
    writes[0].variableId = kVariable;
    writes[0].value = 1;
    writes[1].variableId = otherVariable;
    writes[1].value = 1;
    VnVariableBatchDescV1 batch{};
    batch.structSize = sizeof(batch);
    batch.writes = writes;
    batch.writeCount = 2;
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&batch, &ticket));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_REJECTED_TARGET, QueryTicket(ticket).outcome);
    EXPECT_FLOAT_EQ(0, QueryCurrentValue(kVariable));
    EXPECT_FLOAT_EQ(0, QueryCurrentValue(otherVariable));
    const auto expectLocal = [&](VnEmitterHandle emitter, VnVariableId id, float expected) {
        VnVariableQueryDescV1 query{};
        query.structSize = sizeof(query);
        query.variableId = id;
        query.emitter = emitter;
        VnVariableQueryResultV1 value{};
        value.structSize = sizeof(value);
        ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &value));
        EXPECT_FLOAT_EQ(expected, value.currentValue);
        EXPECT_EQ(1u, value.hasEmitterOverride);
        float projected = 0;
        ASSERT_TRUE(engine->emitters.LocalValue(emitter, id, &projected));
        EXPECT_FLOAT_EQ(expected, projected);
    };
    ASSERT_NO_FATAL_FAILURE(expectLocal(a, kVariable, 0.25f));
    ASSERT_NO_FATAL_FAILURE(expectLocal(b, kVariable, 0.5f));
    ASSERT_NO_FATAL_FAILURE(expectLocal(a, otherVariable, 0.75f));
    EXPECT_FLOAT_EQ(oldGain, engine->voices.Resolve(render)->gain.target);
    EXPECT_FLOAT_EQ(oldResolved, engine->params.ResolvedValue(render, harken::kTargetVoiceVolume));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.5f));
    harkentest::AdvanceVariableWrite(engine);
    float value = 0;
    EXPECT_FALSE(engine->emitters.LocalValue(a, kVariable, &value));
    EXPECT_FALSE(engine->emitters.LocalValue(b, kVariable, &value));
    ASSERT_NO_FATAL_FAILURE(expectLocal(a, otherVariable, 0.75f));
    EXPECT_FLOAT_EQ(2.0f, engine->voices.Resolve(render)->gain.target);
}

TEST(VariableCurrentSlice, FiniteOverflowBusWriteRowIsTargetRejected) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxBuses = 4;
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(9), VN_BUS_MASTER));
    RegisterBusVolumeBinding(harkentest::ObjectId(1), harkentest::ObjectId(9), 0.0f, 0.0f, 1.0f, 772.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    harken::BusTree::Bus* busState =
        engine->buses.BusState(engine->buses.FindIndex(harkentest::ObjectId(9)));
    ASSERT_NE(nullptr, busState);
    ASSERT_FLOAT_EQ(1.0f, busState->gain.target);
    const std::uint64_t commandHead = engine->commands.PublishedHead();

    const VnVariableTicketV1 rejectedTicket = AcceptGlobalWrite(kVariable, 1.0f);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    const auto rejected = QueryTicket(rejectedTicket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, rejected.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_REJECTED_TARGET, rejected.outcome);
    EXPECT_FLOAT_EQ(0.0f, QueryCurrentValue(kVariable));
    EXPECT_FLOAT_EQ(1.0f, busState->gain.target);
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_FALSE(engine->busTargets.ForeignClaim(
        engine->buses.FindIndex(harkentest::ObjectId(9)), harken::kBusTargetBusVolume));

    const VnVariableTicketV1 legalTicket = AcceptGlobalWrite(kVariable, 0.1f);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    const auto applied = QueryTicket(legalTicket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, applied.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_APPLIED, applied.outcome);
    EXPECT_FLOAT_EQ(0.1f, QueryCurrentValue(kVariable));
    EXPECT_FLOAT_EQ(1.0f / 0.9f, busState->gain.target);
}

std::uint32_t VoiceLastPublishedRampMs(harken::Engine* engine,
                                       VnVoiceHandle render,
                                       std::uint32_t targetId) {
    std::uint32_t rampMs = 0;
    engine->params.ForEachActiveTarget(
        [&](const harken::ParamSystem::TargetCaptureRow& row) {
            if (row.handle == render && row.targetId == targetId) {
                rampMs = row.lastPublishedRampMs;
            }
        });
    return rampMs;
}

TEST(VariableCurrentSlice, RejectedCandidateDoesNotPolluteLegalRampMs) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxBuses = 4;
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    const VnVoiceHandle voice = PlayDc(engine, harkentest::ObjectId(1), VN_BUS_MASTER);
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    RegisterLinearBinding(harkentest::ObjectId(1), kVariable, VN_TARGET_SCOPE_VOICE,
                          harken::kTargetVoiceVolume, 0.0f, 800.0f, 500);
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(voice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    engine->params.SetAttenuationSlot(render, harken::kTargetVoiceVolume,
                                      -1.0f, 20);
    EXPECT_EQ(20u, engine->params.PendingRampMs(
                       render, harken::kTargetVoiceVolume));

    const VnVariableTicketV1 rejectedTicket = AcceptGlobalWrite(kVariable, 1.0f);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    const auto rejected = QueryTicket(rejectedTicket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, rejected.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_REJECTED_TARGET, rejected.outcome);
    EXPECT_NE(500u, VoiceLastPublishedRampMs(engine, render,
                                             harken::kTargetVoiceVolume));
    EXPECT_EQ(20u, VoiceLastPublishedRampMs(engine, render,
                                            harken::kTargetVoiceVolume));
    EXPECT_TRUE(std::isfinite(engine->voices.Resolve(render)->gain.target));

    const VnVariableTicketV1 legalTicket = AcceptGlobalWrite(kVariable, 0.0f);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    EXPECT_EQ(VN_VARIABLE_OUTCOME_APPLIED, QueryTicket(legalTicket).outcome);
}

TEST(VariableCurrentSlice, RejectedCandidateKeepsZeroPendingRampMs) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    const VnVoiceHandle voice = PlayDc(engine, harkentest::ObjectId(2), VN_BUS_MASTER);
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    RegisterLinearBinding(harkentest::ObjectId(1), kVariable, VN_TARGET_SCOPE_VOICE,
                          harken::kTargetVoiceVolume, 0.0f, 800.0f, 500);
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(voice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    EXPECT_EQ(0u, engine->params.PendingRampMs(render,
                                               harken::kTargetVoiceVolume));
    const VnVariableTicketV1 rejectedTicket = AcceptGlobalWrite(kVariable, 1.0f);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    EXPECT_EQ(VN_VARIABLE_OUTCOME_REJECTED_TARGET,
              QueryTicket(rejectedTicket).outcome);
    EXPECT_EQ(0u, VoiceLastPublishedRampMs(engine, render,
                                           harken::kTargetVoiceVolume));
    EXPECT_EQ(0u, engine->params.PendingRampMs(render,
                                               harken::kTargetVoiceVolume));
}

TEST(VariableCurrentSlice, RejectedCandidateDoesNotPollutePitchRampMs) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxBindings = 8;
    config.maxBindingAttachments = 8;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    const VnVoiceHandle voice = PlayDc(engine, harkentest::ObjectId(3), VN_BUS_MASTER);
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    RegisterLinearBinding(harkentest::ObjectId(1), kVariable, VN_TARGET_SCOPE_VOICE,
                          harken::kTargetVoiceVolume, 0.0f, 800.0f, 500);
    RegisterLinearBinding(harkentest::ObjectId(2), kVariable, VN_TARGET_SCOPE_VOICE,
                          harken::kTargetVoicePitch, 0.0f, 100.0f, 20);
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(voice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(voice, harkentest::ObjectId(2)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    const VnVariableTicketV1 rejectedTicket = AcceptGlobalWrite(kVariable, 1.0f);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    EXPECT_EQ(VN_VARIABLE_OUTCOME_REJECTED_TARGET,
              QueryTicket(rejectedTicket).outcome);
    EXPECT_NE(500u, VoiceLastPublishedRampMs(engine, render,
                                             harken::kTargetVoicePitch));
    EXPECT_EQ(0u, VoiceLastPublishedRampMs(engine, render,
                                           harken::kTargetVoiceVolume));
}

TEST(VariableCurrentSlice, RejectedBusCandidateDoesNotPolluteLegalRampMs) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxBuses = 4;
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(9), VN_BUS_MASTER));
    RegisterBusVolumeBinding(harkentest::ObjectId(1), harkentest::ObjectId(9), 0.0f, 0.0f, 1.0f, 800.0f, 500);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    const std::uint32_t busIndex = engine->buses.FindIndex(harkentest::ObjectId(9));
    engine->busTargets.SetSlot(busIndex, harken::kBusTargetBusVolume,
                               harken::BusTargetStore::kSlotApiDirectIndex,
                               -1.0f);
    engine->busTargets.MarkVariableChanged(busIndex,
                                           harken::kBusTargetBusVolume, 20);
    EXPECT_EQ(20u, engine->busTargets.PendingRampMs(
                       busIndex, harken::kBusTargetBusVolume));
    const VnVariableTicketV1 rejectedTicket = AcceptGlobalWrite(kVariable, 1.0f);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    EXPECT_EQ(VN_VARIABLE_OUTCOME_REJECTED_TARGET,
              QueryTicket(rejectedTicket).outcome);
    EXPECT_EQ(20u, engine->busTargets.LastPublishedRampMs(
                       busIndex, harken::kBusTargetBusVolume));
    EXPECT_NE(500u, engine->busTargets.LastPublishedRampMs(
                        busIndex, harken::kBusTargetBusVolume));
    const VnVariableTicketV1 legalTicket = AcceptGlobalWrite(kVariable, 0.0f);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    EXPECT_EQ(VN_VARIABLE_OUTCOME_APPLIED, QueryTicket(legalTicket).outcome);
}

TEST(VariableCurrentSlice,
     WaitingFrontierLegalRequestSurvivesRejectedCandidate) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    const VnVoiceHandle voice = PlayDc(engine, harkentest::ObjectId(4), VN_BUS_MASTER);
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    RegisterLinearBinding(harkentest::ObjectId(1), kVariable, VN_TARGET_SCOPE_VOICE,
                          harken::kTargetVoiceVolume, 0.0f, 800.0f, 500);
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(voice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    const std::uint64_t settledBefore = engine->commands.SettledFrontier();
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(voice, -1.0f, 0));
    ASSERT_GT(engine->commands.PublishedHead(), settledBefore);
    engine->mixer.SetMaxCommandsPerQuantumForTest(0);
    const VnVariableTicketV1 rejectedTicket = AcceptGlobalWrite(kVariable, 1.0f);
    engine->nullBackend.Pump(8);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_GT(engine->variables.PendingCount(), 0u);
    ASSERT_TRUE(engine->params.ForeignClaim(render, harken::kTargetVoiceVolume));
    engine->params.SetAttenuationSlot(render, harken::kTargetVoiceVolume,
                                      -1.0f, 20);
    EXPECT_EQ(20u, engine->params.PendingRampMs(
                       render, harken::kTargetVoiceVolume));
    engine->mixer.SetMaxCommandsPerQuantumForTest(UINT32_MAX);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    EXPECT_EQ(VN_VARIABLE_OUTCOME_REJECTED_TARGET,
              QueryTicket(rejectedTicket).outcome);
    EXPECT_EQ(20u, VoiceLastPublishedRampMs(engine, render,
                                            harken::kTargetVoiceVolume));
    EXPECT_NE(500u, VoiceLastPublishedRampMs(engine, render,
                                             harken::kTargetVoiceVolume));
}

} // namespace
