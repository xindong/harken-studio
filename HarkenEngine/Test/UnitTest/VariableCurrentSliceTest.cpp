#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "Common/VariableOutcomeCapture.h"
#include "Common/StreamBuilder.h"
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
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace harken::detail {

void SetParkedReadCutCapturedHookForTest(
    void (*hook)(Engine&, VariableReadCutRef, void*), void* context) noexcept;

struct VariableServiceTestAccess {
    static bool HasHeldPlacements(const VariableServiceV1& service) {
        for (std::size_t slot = 0; slot < service.history_.size(); ++slot)
            if (service.history_[slot].placementHeld || service.preparedWritePlacements_[slot].IsHeld())
                return true;
        return false;
    }
    static bool ReadCutStorageAlive(const VariableServiceV1& service, VariableReadCutRef ref) {
        return service.ResolveReadCut(ref) != nullptr;
    }
    static bool TimelineReadUseStorageAlive(const VariableServiceV1& service, VariableTimelineReadUseRef ref) {
        return service.ResolveTimelineReadUse(ref) != nullptr;
    }
    static bool RebuildPending(VariableServiceV1& service, VariableReadCutRef ref,
        std::uint64_t frame) {
        auto* cut = service.ResolveReadCut(ref);
        return cut != nullptr && service.RebuildPendingReadCut(*cut, frame);
    }
    static std::size_t CandidateCount(const VariableServiceV1& service, VariableReadCutRef ref) {
        const auto* cut = service.ResolveReadCut(ref);
        return cut == nullptr ? 0u : cut->candidatePins.count();
    }
    static std::size_t TotalObservers(const VariableServiceV1& service) {
        std::size_t count = 0;
        for (const auto& row : service.history_) count += row.observers;
        return count;
    }
    static std::size_t HeldPlacementCount(const VariableServiceV1& service) {
        return std::count_if(service.preparedWritePlacements_.begin(),
            service.preparedWritePlacements_.end(), [](const auto& held) { return held.IsHeld(); });
    }
    static std::uint64_t PreparedCount(const VariableServiceV1& service) {
        return service.preparedRows_;
    }
    static std::size_t HistorySlotForOrder(const VariableServiceV1& service,
        LogicCommitSequence sequence, std::uint32_t ordinal) {
        for (std::size_t slot = 0; slot < service.history_.size(); ++slot) {
            const auto& row = service.history_[slot];
            if ((row.used || row.reserved) && row.ticket.sequence == sequence && row.executionOrdinal == ordinal)
                return slot;
        }
        return SIZE_MAX;
    }
    static std::uint32_t ObserversForOrder(const VariableServiceV1& service,
        LogicCommitSequence sequence, std::uint32_t ordinal) {
        for (const auto& row : service.history_)
            if (row.used && row.ticket.sequence == sequence && row.executionOrdinal == ordinal)
                return row.observers;
        return 0;
    }
    static std::uint32_t ObserversForVariable(const VariableServiceV1& service,
        LogicCommitSequence sequence, VnObjectId variableId) {
        for (const auto& row : service.history_)
            if (row.used && row.ticket.sequence == sequence && row.variableId == variableId)
                return row.observers;
        return 0;
    }
    static bool SeedObserversForOrder(VariableServiceV1& service, LogicCommitSequence sequence,
        std::uint32_t ordinal, std::uint32_t observers) {
        for (auto& row : service.history_) {
            if ((row.used || row.reserved) && row.ticket.sequence == sequence && row.executionOrdinal == ordinal) {
                row.observers = observers;
                return true;
            }
        }
        return false;
    }
    static VariableSliceRef PreparedSliceForOrder(const VariableServiceV1& service,
        LogicCommitSequence sequence, std::uint32_t ordinal) {
        for (const auto& row : service.history_) {
            if ((row.used || row.reserved) && row.ticket.sequence == sequence && row.executionOrdinal == ordinal)
                return row.slice;
        }
        return {};
    }
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

bool ReadCutStorageAliveForTest(const VariableServiceV1& service, VariableReadCutRef ref) {
    return VariableServiceTestAccess::ReadCutStorageAlive(service, ref);
}
bool TimelineReadUseStorageAliveForTest(const VariableServiceV1& service, VariableTimelineReadUseRef ref) {
    return VariableServiceTestAccess::TimelineReadUseStorageAlive(service, ref);
}

} // namespace harken::detail

namespace {

using harkentest::EngineSession;
using harkentest::MakePlayDesc;
using harkentest::MakeVariableDefinition;
using harkentest::MonoDesc;
using CurrentSliceHookPhase =
    harken::MixerLoop::CurrentSliceHookPhaseForTest;

constexpr std::uint32_t kBlockFrames = 64;
constexpr std::uint32_t kSampleRate = 48000;
constexpr VnVariableId kVariable = harkentest::ObjectId(7001);

VnConfig CurrentSliceConfig(std::uint32_t maxVoices, std::uint32_t blockFrames,
                           std::uint32_t sampleRate) {
    auto config = harkentest::MakeNullConfig(maxVoices, blockFrames, sampleRate);
    // Direct firing-table proofs do not dispatch nested Action programs.
    config.maxTriggerDepth = 1;
    // Leave room for the root, child and sibling without default-sized attempts.
    config.maxActionFirings = 8;
    // The binding scenarios use at most two buses and one delay instance.
    config.maxBuses = 4;
    config.delayEffectPoolCapacity = 1;
    return config;
}

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

TEST(VariableCurrentSlice, SuccessfulWriteThenDirectPlayUsesNewValueWithoutUpdateWait) {
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    static const std::vector<float> samples(kSampleRate, 0.05f);
    const VnPCMDesc pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &pcm));
    RegisterVoiceAttachment(harkentest::ObjectId(1), harkentest::ObjectId(2));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(2));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_NE(0u, voice);
    ASSERT_EQ(VN_OK, Vn_Update());
    bool heard = false;
    for (std::uint32_t block = 0; block < 16 && !heard; ++block) {
        engine->nullBackend.Pump(1);
        const float* output = engine->nullBackend.LastBlock();
        for (std::uint32_t frame = 0; frame < kBlockFrames; ++frame) {
            if (std::abs(output[frame * 2]) <= 1.0e-7f) continue;
            EXPECT_NEAR(0.05f * DbToLinear(-12.0f),
                        output[frame * 2], 1.0e-5f);
            heard = true;
            break;
        }
    }
    EXPECT_TRUE(heard);
}

TEST(VariableCurrentSlice, ConsecutiveDirectPlaysKeepOwnFirstPcmThenFollowLatestValue) {
    for (const bool stallFifo : {false, true}) {
        SCOPED_TRACE(stallFifo);
        EngineSession session(CurrentSliceConfig(4, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const std::vector<float> samples(kSampleRate, 0.05f);
        const auto pcm = MonoDesc(samples, kSampleRate);
        const auto sound = harkentest::ObjectId(7201);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
        RegisterVoiceAttachment(harkentest::ObjectId(7202), sound);
        const auto panBinding = harkentest::ObjectId(7203);
        RegisterLinearBinding(panBinding, kVariable, VN_TARGET_SCOPE_VOICE,
            harken::kTargetVoicePan, -1, 1);
        VnBindingAttachDesc attach{};
        attach.structSize = sizeof(attach);
        attach.scopeKind = VN_TARGET_SCOPE_VOICE;
        attach.objectId = sound;
        ASSERT_EQ(VN_OK, Vn_AttachBinding(panBinding, &attach));
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(2);
        const auto play = MakePlayDesc(sound);
        VnVoiceHandle first = 0, second = 0;
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0));
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
        ASSERT_EQ(VN_OK, Vn_Update());
        if (stallFifo) {
            e.mixer.SetMaxCommandsPerQuantumForTest(0);
            e.nullBackend.Pump(8);
            for (std::uint32_t i = 0; i < 2 * kBlockFrames; ++i)
                EXPECT_FLOAT_EQ(0, e.nullBackend.LastBlock()[i]);
            e.mixer.SetMaxCommandsPerQuantumForTest(e.config.maxControlCommandsPerQuantum);
        }
        bool heard = false;
        for (std::uint32_t step = 0; step < 16 && !heard; ++step) {
            e.nullBackend.Pump(1);
            const auto* output = e.nullBackend.LastBlock();
            if (std::abs(output[2 * (kBlockFrames - 1)]) < 1e-7f) continue;
            heard = true;
            for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i) {
                EXPECT_NEAR(0.05f * std::sqrt(2.0f), output[2 * i], 1e-5f);
                EXPECT_NEAR(0.05f * DbToLinear(-12) * std::sqrt(2.0f), output[2 * i + 1], 1e-5f);
            }
        }
        ASSERT_TRUE(heard);
        e.nullBackend.Pump(1);
        const auto* firstState = e.voices.Resolve(harkentest::RenderHandle(&e, first));
        ASSERT_NE(nullptr, firstState);
        EXPECT_FLOAT_EQ(-1, firstState->pan);
        EXPECT_FLOAT_EQ(1, firstState->gain.target);
        // The existing pan matrix takes 10 ms to settle after a live retarget.
        e.nullBackend.Pump((kSampleRate / 100 + kBlockFrames - 1) / kBlockFrames);
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i) {
            EXPECT_NEAR(0.1f * std::sqrt(2.0f), e.nullBackend.LastBlock()[2 * i], 1e-5f);
            EXPECT_NEAR(0, e.nullBackend.LastBlock()[2 * i + 1], 1e-5f);
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_FALSE(e.variables.HasNativePublicationHold(harkentest::RenderHandle(&e, first)));
        EXPECT_FALSE(e.variables.HasNativePublicationHold(harkentest::RenderHandle(&e, second)));
    }
}

TEST(VariableCurrentSlice, DirectVolumePreflightUsesCapturedValueAndRefusalReleasesCut) {
    for (const bool newValueOverflows : {false, true}) {
        SCOPED_TRACE(newValueOverflows);
        EngineSession session(CurrentSliceConfig(4, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const std::vector<float> samples(kSampleRate, 1.0e-39f);
        const auto pcm = MonoDesc(samples, kSampleRate);
        const auto sound = harkentest::ObjectId(7211);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
        RegisterVoiceAttachment(harkentest::ObjectId(7212), sound,
            0, newValueOverflows ? 0 : 20, 1, newValueOverflows ? 20 : 0);
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(2);
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        auto play = MakePlayDesc(sound);
        play.volumeDb = 760;
        VnVoiceHandle handle = 0;
        if (newValueOverflows) {
            EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play, &handle));
            EXPECT_EQ(0u, handle);
            play.volumeDb = 0;
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        } else {
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(8);
        ASSERT_EQ(VN_OK, Vn_Update());
        float value = 0;
        ASSERT_TRUE(e.variables.CurrentValue(0, kVariable, &value));
        EXPECT_FLOAT_EQ(1, value);
        EXPECT_FALSE(e.variables.HasNativePublicationHold(harkentest::RenderHandle(&e, handle)));
    }
}

TEST(VariableCurrentSlice, SourceFreeDirectPlayRendersWithoutUpdate) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e, 0, 1, 0.5f);
    const std::vector<float> samples(kSampleRate, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    const auto sound = harkentest::ObjectId(7221);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    RegisterVoiceAttachment(harkentest::ObjectId(7222), sound);
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    const auto play = MakePlayDesc(sound);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    e.nullBackend.Pump(1);
    // Voice-volume curves interpolate amplitude, rather than the dB coordinates.
    const float midpointGain = (1 + DbToLinear(-12)) * 0.5f;
    for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
        EXPECT_NEAR(0.05f * midpointGain,
            e.nullBackend.LastBlock()[2 * i], 1e-6f);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FALSE(e.variables.HasNativePublicationHold(harkentest::RenderHandle(&e, handle)));
}

TEST(VariableCurrentSlice, DirectSourceFailureAndStopReleaseWaitingVoiceAndAllowRetry) {
    for (const bool stop : {false, true}) {
        SCOPED_TRACE(stop);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const std::vector<float> samples(kSampleRate, 0.05f);
        const auto pcm = MonoDesc(samples, kSampleRate);
        const auto sound = harkentest::ObjectId(7231);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
        RegisterVoiceAttachment(harkentest::ObjectId(7232), sound);
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(2);
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        const auto sequence = e.logicCommits.Current();
        const auto play = MakePlayDesc(sound);
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        const auto render = harkentest::RenderHandle(&e, handle);
        ASSERT_TRUE(e.variables.HasNativePublicationHold(render));
        if (stop) {
            ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
        } else {
            const auto source = e.variables.SliceForTicketForTests(sequence);
            ASSERT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim,
                e.variableSlices.Invalidate(source, 1));
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(8);
        for (std::uint32_t i = 0; i < 2 * kBlockFrames; ++i)
            EXPECT_FLOAT_EQ(0, e.nullBackend.LastBlock()[i]);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(nullptr, e.voices.Resolve(render));
        EXPECT_FALSE(e.variables.HasNativePublicationHold(render));
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.5f));
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(8);
        const float midpointGain = (1 + DbToLinear(-12)) * 0.5f;
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f * midpointGain,
                e.nullBackend.LastBlock()[2 * i], 1e-6f);
    }
}

TEST(VariableCurrentSlice, DirectQueueRefusalReleasesCapturedResourcesAndPreservesWrite) {
    auto config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.commandQueueDepth = 64;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto sound = harkentest::ObjectId(7251);
    const std::vector<float> samples(kSampleRate, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    RegisterVoiceAttachment(harkentest::ObjectId(7252), sound);
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
    while (e.commands.PublishedHead() - e.commands.ConsumedCount() < e.commands.Capacity() - 1)
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, 0, 0));
    const auto head = e.commands.PublishedHead();
    const auto demand = e.propertyPublisher.WorstCaseCommits();
    const auto source = e.registry.Lookup(sound);
    ASSERT_NE(nullptr, source.useCount);
    const auto pcmPins = source.useCount->load(std::memory_order_acquire);
    const auto play = MakePlayDesc(sound);
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_BACKPRESSURE, harkentest::PlayRaw(&play, &handle));
    EXPECT_EQ(0u, handle);
    EXPECT_EQ(head, e.commands.PublishedHead());
    EXPECT_EQ(0u, e.voices.ActiveCount());
    EXPECT_EQ(demand, e.propertyPublisher.WorstCaseCommits());
    EXPECT_EQ(pcmPins, source.useCount->load(std::memory_order_acquire));
    e.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(8);
    ASSERT_EQ(VN_OK, Vn_Update());
    float value = 0;
    ASSERT_TRUE(e.variables.CurrentValue(kVariable, &value));
    EXPECT_FLOAT_EQ(1, value);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    e.nullBackend.Pump(1);
    for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
        EXPECT_NEAR(0.05f * DbToLinear(-12),
            e.nullBackend.LastBlock()[2 * i], 1e-6f);
}

TEST(VariableCurrentSlice, DirectStreamStarvationKeepsFirstValueThroughUpdateAndNativeAck) {
    for (const std::uint32_t workers : {0u, 2u}) {
        SCOPED_TRACE(workers);
        auto config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
        config.renderWorkerCount = workers;
        config.prebufferMs = 1;
        config.streamRingFrames = 512;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const auto sound = harkentest::ObjectId(7241);
        harkentest::StreamSpec spec;
        spec.soundId = sound;
        spec.sampleRate = kSampleRate;
        spec.channelCount = 1;
        spec.chunkFrames = 256;
        const std::string path = std::string(HARKEN_TEMP_DIR) + "/r03_direct_starvation.stream";
        harkentest::WriteFileBytes(path, harkentest::BuildStreamBytes(
            spec, std::vector<float>(kSampleRate, 0.05f)));
        ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(sound, path.c_str()));
        RegisterVoiceAttachment(harkentest::ObjectId(7242), sound);
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(2);
        e.streams.SetThrottled(true);
        e.streams.SyncServe();
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        const auto play = MakePlayDesc(sound);
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        const auto render = harkentest::RenderHandle(&e, handle);
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(8);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_FALSE(e.variables.HasNativePublicationHold(render));
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0));
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(8);
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(1);
        ASSERT_NE(nullptr, e.voices.Resolve(render));
        EXPECT_EQ(0u, e.voices.Resolve(render)->cascade.produced);
        EXPECT_FLOAT_EQ(DbToLinear(-12), e.voices.Resolve(render)->gain.target);
        for (std::uint32_t i = 0; i < 2 * kBlockFrames; ++i)
            EXPECT_FLOAT_EQ(0, e.nullBackend.LastBlock()[i]);
        e.streams.SetThrottled(false);
        e.streams.SyncServe();
        e.nullBackend.Pump(1);
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f * DbToLinear(-12),
                e.nullBackend.LastBlock()[2 * i], 1e-6f);
        e.streams.SyncServe();
        e.nullBackend.Pump(1);
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f, e.nullBackend.LastBlock()[2 * i], 1e-6f);
    }
}

TEST(VariableCurrentSlice, DirectSettersSupersedeAcknowledgedVariableWhileStreamStarves) {
    for (const std::uint32_t workers : {0u, 2u}) {
        SCOPED_TRACE(workers);
        auto config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
        config.renderWorkerCount = workers;
        config.prebufferMs = 1;
        config.streamRingFrames = 512;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const auto sound = harkentest::ObjectId(7243);
        harkentest::StreamSpec spec;
        spec.soundId = sound;
        spec.sampleRate = kSampleRate;
        spec.channelCount = 1;
        spec.chunkFrames = 256;
        const std::string path = std::string(HARKEN_TEMP_DIR) + "/r03_direct_setter_starvation.stream";
        harkentest::WriteFileBytes(path, harkentest::BuildStreamBytes(
            spec, std::vector<float>(kSampleRate, 0.05f)));
        ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(sound, path.c_str()));
        RegisterVoiceAttachment(harkentest::ObjectId(7244), sound);
        const auto pitchBinding = harkentest::ObjectId(7245);
        RegisterLinearBinding(pitchBinding, kVariable, VN_TARGET_SCOPE_VOICE,
            harken::kTargetVoicePitch, 0, 1200);
        VnBindingAttachDesc attach{};
        attach.structSize = sizeof(attach);
        attach.scopeKind = VN_TARGET_SCOPE_VOICE;
        attach.objectId = sound;
        ASSERT_EQ(VN_OK, Vn_AttachBinding(pitchBinding, &attach));
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(2);
        e.streams.SetThrottled(true);
        e.streams.SyncServe();
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        const auto play = MakePlayDesc(sound);
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        const auto render = harkentest::RenderHandle(&e, handle);
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(8);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_FALSE(e.variables.HasNativePublicationHold(render));
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0));
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(8);
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(1);
        ASSERT_NE(nullptr, e.voices.Resolve(render));
        ASSERT_EQ(0u, e.voices.Resolve(render)->cascade.produced);
        ASSERT_FLOAT_EQ(DbToLinear(-12), e.voices.Resolve(render)->gain.target);
        ASSERT_DOUBLE_EQ(2, e.voices.Resolve(render)->resampler.TargetRatio());
        ASSERT_FALSE(e.variables.HasNativePublicationHold(render));
        ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, -6, 0));
        ASSERT_EQ(VN_OK, Vn_SetVoicePitch(handle, 1.5f));
        ASSERT_EQ(VN_OK, Vn_SetVoicePan(handle, -1));
        e.nullBackend.Pump(1);
        ASSERT_EQ(0u, e.voices.Resolve(render)->cascade.produced);
        ASSERT_FLOAT_EQ(DbToLinear(-6), e.voices.Resolve(render)->gain.target);
        ASSERT_NEAR(1.5, e.voices.Resolve(render)->resampler.TargetRatio(), 1e-7);
        e.streams.SetThrottled(false);
        for (std::uint32_t block = 0; block < 4; ++block) {
            SCOPED_TRACE(block);
            e.streams.SyncServe();
            e.nullBackend.Pump(1);
            ASSERT_GT(e.voices.Resolve(render)->cascade.produced, 0u);
            EXPECT_FLOAT_EQ(DbToLinear(-6), e.voices.Resolve(render)->gain.target);
            EXPECT_NEAR(1.5, e.voices.Resolve(render)->resampler.TargetRatio(), 1e-7);
            for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
                // The spatial mono law anchors stereo center at unity per side.
                EXPECT_NEAR(0.05f * DbToLinear(-6) * std::sqrt(2.0f),
                    e.nullBackend.LastBlock()[2 * i], 1e-6f);
        }
    }
}

TEST(VariableCurrentSlice, StagedStartsCaptureApplicableWritesAtCommitAndKeepOwnFirstPcm) {
    for (const bool writeBeforeStaging : {false, true}) {
        SCOPED_TRACE(writeBeforeStaging);
        EngineSession session(CurrentSliceConfig(4, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const auto sound = harkentest::ObjectId(7261);
        const std::vector<float> samples(kSampleRate, 0.05f);
        const auto pcm = MonoDesc(samples, kSampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
        RegisterVoiceAttachment(harkentest::ObjectId(7262), sound);
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(2);
        auto play = MakePlayDesc(sound);
        play.reserved0 = VN_PLAY_FLAG_STAGED_INSTALL;
        if (writeBeforeStaging) ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        VnVoiceHandle first = 0, second = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
        ASSERT_EQ(VN_OK, Vn_SetVoicePan(first, -1));
        if (!writeBeforeStaging) ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(first));
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0));
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
        ASSERT_EQ(VN_OK, Vn_SetVoicePan(second, 1));
        ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(second));
        ASSERT_EQ(VN_OK, Vn_Update());
        bool heard = false;
        for (std::uint32_t step = 0; step < 16 && !heard; ++step) {
            e.nullBackend.Pump(1);
            const auto* output = e.nullBackend.LastBlock();
            if (std::abs(output[2 * (kBlockFrames - 1) + 1]) < 1e-7f) continue;
            heard = true;
            for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i) {
                EXPECT_NEAR(0.05f * DbToLinear(-12) * std::sqrt(2.0f), output[2 * i], 1e-6f);
                EXPECT_NEAR(0.05f * std::sqrt(2.0f), output[2 * i + 1], 1e-6f);
            }
        }
        ASSERT_TRUE(heard);
        e.nullBackend.Pump(1);
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f * std::sqrt(2.0f), e.nullBackend.LastBlock()[2 * i], 1e-6f);
    }
}

TEST(VariableCurrentSlice, StagedInvalidCapturedVolumePreservesReservationForRetry) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto sound = harkentest::ObjectId(7271);
    const std::vector<float> samples(kSampleRate, 1.0e-20f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    RegisterVoiceAttachment(harkentest::ObjectId(7272), sound, 0, 0, 1, 400);
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    auto play = MakePlayDesc(sound);
    play.reserved0 = VN_PLAY_FLAG_STAGED_INSTALL;
    play.volumeDb = 380;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const auto source = e.registry.Lookup(sound);
    ASSERT_NE(nullptr, source.useCount);
    const auto pins = source.useCount->load(std::memory_order_acquire);
    const auto demand = e.propertyPublisher.WorstCaseCommits();
    const auto head = e.commands.PublishedHead();
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
    EXPECT_EQ(VN_INVALID_ARG, Vn_CommitVoiceStart(handle));
    EXPECT_EQ(head, e.commands.PublishedHead());
    EXPECT_EQ(demand, e.propertyPublisher.WorstCaseCommits());
    EXPECT_EQ(pins, source.useCount->load(std::memory_order_acquire));
    EXPECT_EQ(1u, e.ledger.PendingStartCount());
    EXPECT_EQ(0u, e.voices.ActiveCount());
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0));
    ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(8);
    for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
        EXPECT_NEAR(samples[0] * DbToLinear(380),
            e.nullBackend.LastBlock()[2 * i], 1e-5f);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, e.ledger.PendingStartCount());
}

TEST(VariableCurrentSlice, StagedSourceFailureOrStopReleasesNativeReceiptAndAllowsRetry) {
    for (const bool stop : {false, true}) {
        SCOPED_TRACE(stop);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const auto sound = harkentest::ObjectId(7281);
        const std::vector<float> samples(kSampleRate, 0.05f);
        const auto pcm = MonoDesc(samples, kSampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
        RegisterVoiceAttachment(harkentest::ObjectId(7282), sound);
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(2);
        auto play = MakePlayDesc(sound);
        play.reserved0 = VN_PLAY_FLAG_STAGED_INSTALL;
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        const auto sequence = e.logicCommits.Current();
        ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(handle));
        const auto render = harkentest::RenderHandle(&e, handle);
        ASSERT_TRUE(e.variables.HasNativePublicationHold(render));
        if (stop) {
            ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
        } else {
            const auto source = e.variables.SliceForTicketForTests(sequence);
            ASSERT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim,
                e.variableSlices.Invalidate(source, 1));
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(8);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(nullptr, e.voices.Resolve(render));
        EXPECT_FALSE(e.variables.HasNativePublicationHold(render));
        EXPECT_EQ(0u, e.ledger.PendingStartCount());
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.5f));
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(handle));
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(8);
        const float midpointGain = (1 + DbToLinear(-12)) * 0.5f;
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f * midpointGain,
                e.nullBackend.LastBlock()[2 * i], 1e-6f);
    }
}

TEST(VariableCurrentSlice, LatePendingStartPublishesSourcesAndCalendarInSameUpdate) {
    for (const bool stream : {false, true}) {
        SCOPED_TRACE(stream);
        auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
        config.voiceStealPolicy = 1;
        config.prebufferMs = 1;
        config.streamRingFrames = 512;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const auto sound = harkentest::ObjectId(7291);
        const std::vector<float> samples(kSampleRate, 0.05f);
        const auto pcm = MonoDesc(samples, kSampleRate);
        if (stream) {
            harkentest::StreamSpec spec;
            spec.soundId = sound;
            spec.sampleRate = kSampleRate;
            spec.channelCount = 1;
            spec.chunkFrames = 256;
            const std::string path = std::string(HARKEN_TEMP_DIR) + "/r03_pending.stream";
            harkentest::WriteFileBytes(path, harkentest::BuildStreamBytes(spec, samples));
            ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(sound, path.c_str()));
        } else {
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
        }
        RegisterVoiceAttachment(harkentest::ObjectId(7292), sound);
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(2);
        const auto blocker = PlayDc(&e, harkentest::ObjectId(7293), VN_BUS_MASTER);
        ASSERT_NE(0u, blocker);
        e.nullBackend.Pump(1);
        auto play = MakePlayDesc(sound);
        play.priority = 255;
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        ASSERT_EQ(1u, e.ledger.PendingStartCount());
        e.nullBackend.Pump(3);
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(0u, e.ledger.PendingStartCount());
        const auto render = harkentest::RenderHandle(&e, handle);
        ASSERT_NE(0u, render);
        bool heard = false;
        for (std::uint32_t step = 0; step < 16 && !heard; ++step) {
            if (stream) e.streams.SyncServe();
            e.nullBackend.Pump(1);
            const auto* output = e.nullBackend.LastBlock();
            if (std::abs(output[2 * (kBlockFrames - 1)]) < 1e-7f) continue;
            heard = true;
            for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
                EXPECT_NEAR(0.05f * DbToLinear(-12), output[2 * i], 1e-6f);
        }
        ASSERT_TRUE(heard) << "late start needs no second Vn_Update";
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_FALSE(e.variables.HasNativePublicationHold(render));
        EXPECT_EQ(0u, e.pendingStreamReservations);
        EXPECT_EQ(0u, e.ledger.PendingStartCount());
    }
}

TEST(VariableCurrentSlice, FreshStealAcceptsWriteAndRecapturesAtActualMaterialization) {
    auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
    config.voiceStealPolicy = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto sound = harkentest::ObjectId(7301);
    const std::vector<float> samples(kSampleRate, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    RegisterVoiceAttachment(harkentest::ObjectId(7302), sound);
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    ASSERT_NE(0u, PlayDc(&e, harkentest::ObjectId(7303), VN_BUS_MASTER));
    e.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
    auto play = MakePlayDesc(sound);
    play.priority = 255;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(1u, e.ledger.PendingStartCount());
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0));
    e.nullBackend.Pump(3);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(0u, e.ledger.PendingStartCount());
    bool heard = false;
    for (std::uint32_t step = 0; step < 16 && !heard; ++step) {
        e.nullBackend.Pump(1);
        const auto* output = e.nullBackend.LastBlock();
        if (std::abs(output[2 * (kBlockFrames - 1)]) < 1e-7f) continue;
        heard = true;
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f, output[2 * i], 1e-6f);
    }
    ASSERT_TRUE(heard);
}

TEST(VariableCurrentSlice, FreshVirtualDecisionUsesApplicableValueWithoutRenderClaim) {
    auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -8;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto sound = harkentest::ObjectId(7311);
    const std::vector<float> samples(kSampleRate, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    RegisterVoiceAttachment(harkentest::ObjectId(7312), sound);
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    const auto head = e.commands.PublishedHead();
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
    const auto play = MakePlayDesc(sound);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const auto classified = e.ledger.Classify(handle);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, classified.kind);
    EXPECT_EQ(head, e.commands.PublishedHead());
    EXPECT_EQ(0u, e.voices.ActiveCount());
    EXPECT_FLOAT_EQ(-12, e.parkRecords[classified.ledgerSlot].instanceVolumeDb);
    EXPECT_EQ(1u, e.variables.PendingCount());
    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    for (std::uint32_t step = 0; step < 16 && e.variables.PendingCount() != 0; ++step) {
        e.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(0u, e.variables.PendingCount());
    float value = 0;
    ASSERT_TRUE(e.variables.CurrentValue(kVariable, &value));
    EXPECT_FLOAT_EQ(1, value);
}

TEST(VariableCurrentSlice, StagedVirtualDecisionUsesValueAtCommit) {
    auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -8;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto sound = harkentest::ObjectId(7321);
    const std::vector<float> samples(kSampleRate, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    RegisterVoiceAttachment(harkentest::ObjectId(7322), sound);
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    auto play = MakePlayDesc(sound);
    play.reserved0 = VN_PLAY_FLAG_STAGED_INSTALL;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
    ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(handle));
    const auto classified = e.ledger.Classify(handle);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, classified.kind);
    EXPECT_FLOAT_EQ(-12, e.parkRecords[classified.ledgerSlot].instanceVolumeDb);
    EXPECT_EQ(0u, e.voices.ActiveCount());
}

TEST(VariableCurrentSlice, ParkedReturnUsesNewValueWithoutWaitingForBankAck) {
    auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -8;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e, 0, 1, 1);
    const auto sound = harkentest::ObjectId(7331);
    const std::vector<float> samples(kSampleRate, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    RegisterVoiceAttachment(harkentest::ObjectId(7332), sound);
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    const auto play = MakePlayDesc(sound);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, e.ledger.Classify(handle).kind);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, e.ledger.Classify(handle).kind);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.5f));
    const auto laterSequence = e.logicCommits.Current();
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, e.ledger.Classify(handle).kind);
    bool heard = false;
    for (std::uint32_t step = 0; step < 16 && !heard; ++step) {
        e.nullBackend.Pump(1);
        const auto* output = e.nullBackend.LastBlock();
        if (std::abs(output[2 * (kBlockFrames - 1)]) < 1e-7f) continue;
        heard = true;
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f, output[2 * i], 1e-6f);
    }
    ASSERT_TRUE(heard);
    harken::VariableSliceRef laterSource{};
    for (std::uint32_t step = 0; step < 16; ++step) {
        laterSource = e.variables.SliceForTicketForTests(laterSequence);
        const auto state = e.variableSlices.State(laterSource);
        if (state == harken::VariableSliceState::PageCommitted ||
            state == harken::VariableSliceState::SliceApplied) break;
        e.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, e.ledger.Classify(handle).kind);
    }
    ASSERT_TRUE(laterSource.Valid());
    for (std::uint32_t step = 0; step < 16 &&
         e.variableSlices.State(laterSource) != harken::VariableSliceState::SliceApplied; ++step) {
        e.nullBackend.Pump(1);
        if (e.variableSlices.State(laterSource) != harken::VariableSliceState::SliceApplied)
            ASSERT_EQ(VN_OK, Vn_Update());
    }
    ASSERT_EQ(harken::VariableSliceState::SliceApplied, e.variableSlices.State(laterSource));
    e.nullBackend.Pump(1);
    for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
        EXPECT_NEAR(0.05f * (1 + DbToLinear(-12)) * 0.5f,
            e.nullBackend.LastBlock()[2 * i], 1e-6f);
}

TEST(VariableCurrentSlice, ParkedFailedReadKeepsElapsedTimeForNextSuccessfulAdvance) {
    auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -8;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e, 0, 1, 1);
    const auto sound = harkentest::ObjectId(7341);
    const std::vector<float> samples(kSampleRate, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    RegisterVoiceAttachment(harkentest::ObjectId(7342), sound);
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    const auto play = MakePlayDesc(sound);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const auto classified = e.ledger.Classify(handle);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, classified.kind);
    auto& rec = e.parkRecords[classified.ledgerSlot];
    const auto lastFrame = rec.lastAdvanceFrame;
    const auto cursor = rec.virtualCursor;
    e.nullBackend.Pump(3);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.75f));
    struct HookState { std::uint64_t sequence; bool called = false; } hook{e.logicCommits.Current()};
    harken::detail::SetParkedReadCutCapturedHookForTest(
        [](harken::Engine& engine, harken::VariableReadCutRef, void* opaque) {
            auto& state = *static_cast<HookState*>(opaque);
            state.called = true;
            harken::detail::SetParkedReadCutCapturedHookForTest(nullptr, nullptr);
            EXPECT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim,
                engine.variableSlices.Invalidate(engine.variables.SliceForTicketForTests(state.sequence), 1));
        }, &hook);
    const auto result = Vn_Update();
    harken::detail::SetParkedReadCutCapturedHookForTest(nullptr, nullptr);
    ASSERT_EQ(VN_OK, result);
    ASSERT_TRUE(hook.called);
    EXPECT_EQ(lastFrame, rec.lastAdvanceFrame);
    EXPECT_DOUBLE_EQ(cursor, rec.virtualCursor);
    e.nullBackend.Pump(8);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, e.ledger.Classify(handle).kind);
    EXPECT_EQ(e.serviceFrameClock, rec.lastAdvanceFrame);
    EXPECT_DOUBLE_EQ(cursor + static_cast<double>(e.serviceFrameClock - lastFrame), rec.virtualCursor);
}

TEST(VariableCurrentSlice, ParkedRawBindingPartialRefusalRollsBackAndRetriesWithoutDuplication) {
    auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -8;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto sound = harkentest::ObjectId(7351);
    const std::vector<float> samples(kSampleRate, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    VnVoiceHandle handle = 0;
    const auto play = MakePlayDesc(sound);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const auto originalRender = harkentest::RenderHandle(&e, handle);
    ASSERT_TRUE(e.params.BindVoiceTarget(kVariable, originalRender, harken::kTargetVoiceVolume,
        LinearCurve(0, 0, 1, -3)));
    ASSERT_TRUE(e.params.BindVoiceTarget(kVariable, originalRender, harken::kTargetVoicePitch,
        LinearCurve(0, 0, 1, 1200)));
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, -20, 0));
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto parked = e.ledger.Classify(handle);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parked.kind);
    e.nullBackend.Pump(1);
    VnVoiceHandle temporary = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &temporary));
    e.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Stop(temporary, 0));
    e.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(0u, e.propertyPublisher.WorstCaseCommits());
    const auto pressure = e.propertyPublisher.BatchCapacity() - 1;
    ASSERT_TRUE(e.propertyPublisher.TryGrowWorstCase(pressure, &e.logicLogRing));
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 0, 0));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
    const auto head = e.commands.PublishedHead();
    const auto pins = e.parkRecords[parked.ledgerSlot].facts.view.useCount->load();
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, e.ledger.Classify(handle).kind);
    EXPECT_EQ(head, e.commands.PublishedHead());
    EXPECT_EQ(pressure, e.propertyPublisher.WorstCaseCommits());
    EXPECT_EQ(pins, e.parkRecords[parked.ledgerSlot].facts.view.useCount->load());
    std::uint32_t retryGeneration = 0;
    ASSERT_TRUE(e.voices.FreeSlotGeneration(0, &retryGeneration));
    const auto retryRender = static_cast<VnVoiceHandle>(retryGeneration) << 32;
    EXPECT_EQ(0u, e.params.VoiceBindingCount(retryRender));
    e.propertyPublisher.ShrinkWorstCase(pressure);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, e.ledger.Classify(handle).kind);
    EXPECT_EQ(retryRender, harkentest::RenderHandle(&e, handle));
    EXPECT_EQ(2u, e.params.VoiceBindingCount(retryRender));
    e.nullBackend.Pump(8);
    const auto* ctx = e.voices.Resolve(retryRender);
    ASSERT_NE(nullptr, ctx);
    EXPECT_NEAR(DbToLinear(-3), ctx->gain.target, 1e-6f);
    EXPECT_NEAR(1200, e.params.ResolvedValue(retryRender, harken::kTargetVoicePitch), 1e-6f);
    for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
        EXPECT_NEAR(0.05f * DbToLinear(-3), e.nullBackend.LastBlock()[2 * i], 1e-6f);
}

TEST(VariableCurrentSlice, ParkedNonRecipeEffectsReduceEveryBindingFromReturnCut) {
    auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -8;
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto handle = PlayDc(&e, harkentest::ObjectId(7361), VN_BUS_MASTER);
    ASSERT_NE(0u, handle);
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN,
        reinterpret_cast<const VnEffectDesc*>(&gain)));
    for (const auto id : {harkentest::ObjectId(7362), harkentest::ObjectId(7363)}) {
        RegisterLinearBinding(id, kVariable, VN_TARGET_SCOPE_VOICE_EFFECT_PARAM,
            harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0, harken::kGainParamGainDb), 0, -6);
        ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, id));
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, -20, 0));
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, e.ledger.Classify(handle).kind);
    e.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 0, 0));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, e.ledger.Classify(handle).kind);
    const auto render = harkentest::RenderHandle(&e, handle);
    bool heard = false;
    for (std::uint32_t step = 0; step < 16 && !heard; ++step) {
        e.nullBackend.Pump(1);
        heard = std::abs(e.nullBackend.LastBlock()[2 * (kBlockFrames - 1)]) > 1e-7f;
    }
    ASSERT_TRUE(heard);
    const auto* ctx = e.voices.Resolve(render);
    ASSERT_NE(nullptr, ctx);
    ASSERT_EQ(VN_EFFECT_GAIN, ctx->fxSlotEffectRef[0].type);
    const auto* state = static_cast<const harken::GainState*>(ctx->fxSlots[0].state);
    ASSERT_NE(nullptr, state);
    EXPECT_NEAR(DbToLinear(-12), state->targetGainLinear, 1e-6f);
    e.nullBackend.Pump(8);
    for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
        EXPECT_NEAR(0.05f * DbToLinear(-12),
            e.nullBackend.LastBlock()[2 * i], 1e-6f);
}

TEST(VariableCurrentSlice, StreamPressureReturnKeepsCapturedFirstPcmThroughArenaRefusal) {
    for (const auto workers : {0u, 2u}) {
        SCOPED_TRACE(workers);
        auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
        config.maxParkedVoices = 1;
        config.audibilityParkEnabled = 1;
        config.voiceStealPolicy = 2;
        config.prebufferMs = 1;
        config.streamRingFrames = 512;
        config.renderWorkerCount = workers;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const auto sound = harkentest::ObjectId(7371);
        const std::vector<float> samples(kSampleRate, 0.05f);
        harkentest::StreamSpec spec;
        spec.soundId = sound;
        spec.sampleRate = kSampleRate;
        spec.channelCount = 1;
        spec.chunkFrames = 256;
        const std::string path = std::string(HARKEN_TEMP_DIR) + "/r03_pressure_return.stream";
        harkentest::WriteFileBytes(path, harkentest::BuildStreamBytes(spec, samples));
        ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(sound, path.c_str()));
        RegisterVoiceAttachment(harkentest::ObjectId(7372), sound);
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(2);
        const auto play = MakePlayDesc(sound);
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        e.streams.SyncServe();
        e.nullBackend.Pump(3);
        const auto claimant = PlayDc(&e, harkentest::ObjectId(7373), VN_BUS_MASTER);
        ASSERT_NE(0u, claimant);
        e.streams.SyncServe();
        ASSERT_EQ(VN_OK, Vn_Update());
        const auto parked = e.ledger.Classify(handle);
        ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parked.kind);
        ASSERT_EQ(harken::ParkRecord::State::StreamResumeReady, e.parkRecords[parked.ledgerSlot].state);
        const auto resumeFrame = e.parkRecords[parked.ledgerSlot].streamResumeFrame;
        ASSERT_GT(resumeFrame, 0u);
        e.streams.SetThrottled(true);
        ASSERT_EQ(VN_OK, Vn_Stop(claimant, 0));
        e.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, e.ledger.Classify(handle).kind);
        const auto render = harkentest::RenderHandle(&e, handle);
        const auto* ctx = e.voices.Resolve(render);
        ASSERT_NE(nullptr, ctx);
        EXPECT_TRUE(ctx->streamGateOpen);
        EXPECT_EQ(0u, ctx->streamActivationStartFrame);
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0));
        const auto laterSequence = e.logicCommits.Current();
        ASSERT_EQ(VN_OK, Vn_Update());
        e.mixer.ArenaForTests().SetCapacityForTests(0);
        for (std::uint32_t step = 0; step < 8; ++step) {
            e.nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
            EXPECT_FLOAT_EQ(0, e.nullBackend.LastBlock()[2 * (kBlockFrames - 1)]);
        }
        e.mixer.ArenaForTests().RestoreCapacityForTests();
        e.streams.SetThrottled(false);
        e.streams.SyncServe();
        bool heard = false;
        for (std::uint32_t step = 0; step < 16 && !heard; ++step) {
            e.nullBackend.Pump(1);
            heard = std::abs(e.nullBackend.LastBlock()[2 * (kBlockFrames - 1)]) > 1e-7f;
        }
        ASSERT_TRUE(heard);
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f * DbToLinear(-12),
                e.nullBackend.LastBlock()[2 * i], 1e-6f);
        for (std::uint32_t step = 0; step < 16 &&
             !e.variables.SliceForTicketForTests(laterSequence).Valid(); ++step) {
            e.streams.SyncServe();
            e.nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        e.streams.SyncServe();
        e.nullBackend.Pump(8);
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f, e.nullBackend.LastBlock()[2 * i], 1e-6f);
    }
}

TEST(VariableCurrentSlice, VoiceAndBusTargetsApplyInOneAudioSlice) {
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
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

TEST(VariableCurrentSlice, SharedPublicationSealsGlobalAndEmitterWritesTogetherAndCancelsOnlyOne) {
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    InstallVariable(engine);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const auto voice = PlayDc(engine, harkentest::ObjectId(147), VN_BUS_MASTER, emitter);
    const auto render = harkentest::RenderHandle(engine, voice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(kVariable, render,
        harken::kTargetVoiceVolume, LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    auto observation = harken::ObserveVariableCallAt(
        engine->variables.CurrentWorldGeneration(), engine->variables.CurrentTransportEpoch(),
        kBlockFrames, engine->timelinePool.WindowStartFrame(), engine->timelinePool.PublishCutoffFrame());
    observation.requestedEffectiveFrame = engine->mixer.BlocksRendered() * kBlockFrames;
    const auto global = engine->variables.AcceptGlobal(kVariable, 1.0f, observation,
        engine->params, engine->bindings, engine->busTargets);
    const auto local = engine->variables.AcceptEmitter(emitter, kVariable, 0.0f, observation,
        engine->emitters, engine->params);
    ASSERT_EQ(VN_OK, global.result);
    ASSERT_EQ(VN_OK, local.result);
    engine->variables.BeginService(*engine, engine->mixer.BlocksRendered(), kBlockFrames);
    engine->variables.PrepareClaimedSlices(*engine);
    const auto a = engine->variables.SliceForTicketForTests(global.ticket.sequence);
    const auto b = engine->variables.SliceForTicketForTests(local.ticket.sequence);
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(a));
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(b));
    const auto& slices = std::as_const(engine->variableSlices);
    ASSERT_EQ(1u, slices.TargetCount(a));
    ASSERT_EQ(1u, slices.TargetCount(b));
    EXPECT_FLOAT_EQ(-12.0f, slices.Target(a, 0)->resolvedValue);
    EXPECT_FLOAT_EQ(0.0f, slices.Target(b, 0)->resolvedValue);
    EXPECT_FLOAT_EQ(0.0f, engine->params.ResolvedValue(render, harken::kTargetVoiceVolume));
    ASSERT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim,
        engine->variableSlices.Invalidate(b, 1u));
    CurrentSliceFrontierAdvance frontier{engine};
    engine->mixer.SetCurrentSliceFrontierHookForTest(AdvanceThroughStagedCurrentSlicePage, &frontier);
    for (std::uint32_t i = 0; i < 16 && engine->variables.PendingCount() != 0; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    engine->mixer.SetCurrentSliceFrontierHookForTest(nullptr, nullptr);
    EXPECT_EQ(0u, engine->variables.PendingCount());
    float value = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, kVariable, &value));
    EXPECT_FLOAT_EQ(1.0f, value);
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f), engine->voices.Resolve(render)->gain.target);
    EXPECT_FALSE(engine->params.ForeignClaim(render, harken::kTargetVoiceVolume));
}

TEST(VariableCurrentSlice, LaterVariableSnapshotCombinesTheAcceptedPrefixOnOnePhysicalTarget) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto secondVariable = harkentest::ObjectId(7002);
    const harken::VariableDefinitionV1 definitions[] = {
        MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f),
        MakeVariableDefinition(secondVariable, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(engine, definitions, 2).status);
    const auto voice = PlayDc(engine, harkentest::ObjectId(148), VN_BUS_MASTER);
    const auto render = harkentest::RenderHandle(engine, voice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(kVariable, render,
        harken::kTargetVoiceVolume, LinearCurve(0.0f, 0.0f, 1.0f, -6.0f)));
    ASSERT_TRUE(engine->params.BindVoiceTarget(secondVariable, render,
        harken::kTargetVoiceVolume, LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    auto observation = harken::ObserveVariableCallAt(
        engine->variables.CurrentWorldGeneration(), engine->variables.CurrentTransportEpoch(),
        kBlockFrames, engine->timelinePool.WindowStartFrame(), engine->timelinePool.PublishCutoffFrame());
    observation.requestedEffectiveFrame = engine->mixer.BlocksRendered() * kBlockFrames;
    const auto a = engine->variables.AcceptGlobal(kVariable, 1.0f, observation,
        engine->params, engine->bindings, engine->busTargets);
    const auto b = engine->variables.AcceptGlobal(secondVariable, 1.0f, observation,
        engine->params, engine->bindings, engine->busTargets);
    ASSERT_EQ(VN_OK, a.result);
    ASSERT_EQ(VN_OK, b.result);
    engine->variables.BeginService(*engine, engine->mixer.BlocksRendered(), kBlockFrames);
    engine->variables.PrepareClaimedSlices(*engine);
    const auto aRef = engine->variables.SliceForTicketForTests(a.ticket.sequence);
    const auto bRef = engine->variables.SliceForTicketForTests(b.ticket.sequence);
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(aRef));
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(bRef));
    const auto& slices = std::as_const(engine->variableSlices);
    ASSERT_NE(nullptr, slices.Target(aRef, 0));
    ASSERT_NE(nullptr, slices.Target(bRef, 0));
    EXPECT_FLOAT_EQ(-6.0f, slices.Target(aRef, 0)->resolvedValue);
    EXPECT_FLOAT_EQ(-18.0f, slices.Target(bRef, 0)->resolvedValue);
    EXPECT_FLOAT_EQ(0.0f, engine->params.ResolvedValue(render, harken::kTargetVoiceVolume));
}

TEST(VariableCurrentSlice, NonalignedBlockedPrefixKeepsSharedPhysicalTargetPrivate) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    const auto other = harkentest::ObjectId(7002);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallSmoothDefinitions(&e, {kVariable, other}, 0, 1).status);
    const auto voice = PlayDc(&e, harkentest::ObjectId(149), VN_BUS_MASTER);
    const auto render = harkentest::RenderHandle(&e, voice);
    ASSERT_TRUE(e.params.BindVoiceTarget(kVariable, render, harken::kTargetVoiceVolume,
        LinearCurve(0, 0, 1, -6)));
    ASSERT_TRUE(e.params.BindVoiceTarget(other, render, harken::kTargetVoiceVolume,
        LinearCurve(0, 0, 1, -12)));
    RegisterBusVolumeBinding(harkentest::ObjectId(7101), VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());

    const auto bus = e.buses.FindIndex(VN_BUS_MASTER);
    harken::VariableSliceRef blocker{};
    ASSERT_TRUE(e.variableSlices.Reserve(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), 0xD00D, 0, 0xD00D, 1, &blocker));
    ASSERT_TRUE(e.busTargets.TryClaim(bus, harken::kBusTargetBusVolume, blocker,
        e.busTargets.TargetRevision(bus, harken::kBusTargetBusVolume)));
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    const auto base = e.variables.EarliestReadCutFrame(e) + 2u * kBlockFrames;
    observed.requestedEffectiveFrame = base + 3u;
    const auto a = e.variables.AcceptGlobal(kVariable, 1, observed,
        e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, a.result);
    observed.requestedEffectiveFrame = base + 9u;
    const auto b = e.variables.AcceptGlobal(other, 1, observed,
        e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, b.result);
    e.variables.BeginService(e, e.mixer.BlocksRendered(), kBlockFrames);
    e.variables.PrepareClaimedSlices(e);
    const auto blockedB = e.variables.SliceForTicketForTests(b.ticket.sequence);
    EXPECT_NE(harken::VariableSliceState::PageCommitted, e.variableSlices.State(blockedB));
    EXPECT_EQ(0u, e.variables.AppliedCount());
    e.busTargets.ReleaseClaim(bus, harken::kBusTargetBusVolume, blocker);
    ASSERT_TRUE(e.variableSlices.AbandonPrepared(blocker));

    e.variables.BeginService(e, e.mixer.BlocksRendered(), kBlockFrames);
    e.variables.PrepareClaimedSlices(e);
    const auto sourceA = e.variables.SliceForTicketForTests(a.ticket.sequence);
    const auto sourceB = e.variables.SliceForTicketForTests(b.ticket.sequence);
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(sourceA));
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(sourceB));
    EXPECT_EQ(base + 3u, e.variableSlices.EffectiveFrame(sourceA));
    EXPECT_EQ(base + 9u, e.variableSlices.EffectiveFrame(sourceB));
    const auto& slices = std::as_const(e.variableSlices);
    ASSERT_EQ(1u, slices.TargetCount(sourceB));
    EXPECT_FLOAT_EQ(-18, slices.Target(sourceB, 0)->resolvedValue);
    EXPECT_EQ(1u, slices.SourceCount(sourceB));
    EXPECT_FLOAT_EQ(0, e.params.ResolvedValue(render, harken::kTargetVoiceVolume));
}

TEST(VariableCurrentSlice,
     TargetPoolShortageKeepsCandidateInvisibleAndRetriesTheSameOrdinal) {
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = 2;
    config.maxBindingAttachments = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
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
    ASSERT_TRUE(capturedOutcomes.Pop(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
    EXPECT_EQ(1u, outcome.bankVersionStableOrdinal);
}

TEST(VariableCurrentSlice,
     PermanentCensusOverflowTerminatesAndLetsOtherWritesProgress) {
    constexpr std::uint32_t kVoiceCount = 32;
    constexpr std::uint32_t kTargetsPerVoice = 8;
    constexpr std::array<float, kTargetsPerVoice> initial = {
        0.0f, 0.0f, 20000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    constexpr std::array<float, kTargetsPerVoice> final = {
        -6.0f, 100.0f, 1000.0f, 1000.0f, 0.5f, 0.75f, 6.0f, 1.4f};
    const VnVariableId overlap = harkentest::OffsetObjectId(kVariable, 1);
    const VnVariableId independent = harkentest::OffsetObjectId(kVariable, 2);
    const VnBusId secondBus = harkentest::ObjectId(9702);
    VnConfig config = CurrentSliceConfig(kVoiceCount, kBlockFrames, kSampleRate);
    config.commandQueueDepth = 512;
    config.maxControlCommandsPerQuantum = 512;
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    config.voiceBindingInstanceSlots = kTargetsPerVoice;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const std::array definitions = {
        MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f),
        MakeVariableDefinition(overlap, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f),
        MakeVariableDefinition(independent, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()), 0x43454E4F).status);
    ASSERT_EQ(256u, engine->variableSlices.MaxTargetsPerSlice());
    ASSERT_EQ(VN_OK, Vn_CreateBus(secondBus, VN_BUS_MASTER));
    VnVoiceHandle lastVoice = 0;
    for (std::uint32_t voice = 0; voice < kVoiceCount; ++voice) {
        lastVoice = PlayDc(engine, harkentest::ObjectId(9800 + voice), VN_BUS_MASTER);
        const auto render = harkentest::RenderHandle(engine, lastVoice);
        for (std::uint32_t target = 0; target < kTargetsPerVoice; ++target) {
            ASSERT_TRUE(engine->params.BindVoiceTarget(kVariable, render, target,
                LinearCurve(0.0f, initial[target], 1.0f, final[target])));
        }
    }
    const auto attach = [&](VnBindingId binding, VnVariableId variable, VnBusId bus) {
        RegisterLinearBinding(binding, variable, VN_TARGET_SCOPE_BUS,
                              harken::kBusTargetBusVolume, 0.0f, -6.0f);
        VnBindingAttachDesc desc{};
        desc.structSize = sizeof(desc);
        desc.scopeKind = VN_TARGET_SCOPE_BUS;
        desc.objectId = bus;
        ASSERT_EQ(VN_OK, Vn_AttachBinding(binding, &desc));
    };
    attach(harkentest::ObjectId(9710), kVariable, VN_BUS_MASTER);
    attach(harkentest::ObjectId(9711), overlap, VN_BUS_MASTER);
    attach(harkentest::ObjectId(9712), independent, secondBus);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    const auto firstTicket = engine->logicCommits.IssuedCount() + 1;
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(overlap, 1.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(independent, 1.0f));
    MakeAcceptedWriteDue(engine);
    EXPECT_FALSE(engine->variables.SliceForTicketForTests(firstTicket).Valid());
    const auto blocked = engine->variables.SliceForTicketForTests(firstTicket + 1);
    ASSERT_TRUE(blocked.Valid());
    EXPECT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(blocked));
    const auto separate = engine->variables.SliceForTicketForTests(firstTicket + 2);
    ASSERT_TRUE(separate.Valid());
    EXPECT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(separate));
    engine->nullBackend.Pump(8);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(2u, engine->variables.AppliedCount());
    float value = -1.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(overlap, &value));
    EXPECT_FLOAT_EQ(1.0f, value);
    ASSERT_TRUE(engine->variables.CurrentValue(independent, &value));
    EXPECT_FLOAT_EQ(1.0f, value);
    ASSERT_EQ(VN_OK, Vn_Stop(lastVoice, 0));
    for (std::uint32_t step = 0; step < 32 && engine->variables.PendingCount() != 0; ++step) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(2u, engine->variables.AppliedCount());
    EXPECT_FLOAT_EQ(-6.0f, engine->busTargets.LastCommitted(0, harken::kBusTargetBusVolume));
}

TEST(VariableCurrentSlice,
     UnpreparedPriorBlocksCompletePhysicalTupleButNotIndependentBus) {
    struct Pair {
        std::uint32_t effect;
        std::uint32_t first;
        std::uint32_t second;
        float first0;
        float first1;
        float second0;
        float second1;
    };
    const std::array pairs = {
        Pair{0, harken::kTargetVoiceLowpassQ, harken::kTargetVoiceLowpassCutoff, 0, 1, 20000, 1000},
        Pair{0, harken::kTargetVoiceHighpassQ, harken::kTargetVoiceHighpassCutoff, 0, 1, 0, 1000},
        Pair{0, harken::kTargetVoiceLowShelfQ, harken::kTargetVoiceLowShelfGainDb, 0, 1, 0, 6},
        Pair{0, harken::kTargetVoiceHighShelfQ, harken::kTargetVoiceHighShelfGainDb, 0, 1, 0, 6},
        Pair{VN_EFFECT_THREE_BAND_TONE, harken::kToneParamLowShelfGainDb, harken::kToneParamLowShelfFreqHz, 0, 6, 0, 1},
        Pair{VN_EFFECT_THREE_BAND_TONE, harken::kToneParamPeakQ, harken::kToneParamPeakGainDb, 0, 1, 0, 6},
        Pair{VN_EFFECT_THREE_BAND_TONE, harken::kToneParamHighShelfGainDb, harken::kToneParamHighShelfFreqHz, 0, 6, 0, -1},
        Pair{VN_EFFECT_COMPRESSOR, harken::kCompressorParamThresholdDb, harken::kCompressorParamRatio, 0, -6, 0, 2}};
    for (const auto& pair : pairs) {
        SCOPED_TRACE(::testing::Message() << pair.effect << ':' << pair.first << ':' << pair.second);
        const VnVariableId second = harkentest::OffsetObjectId(kVariable, 1);
        const VnVariableId independent = harkentest::OffsetObjectId(kVariable, 2);
        VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
        config.maxBindings = 4;
        config.maxBindingAttachments = 4;
        config.voiceBindingInstanceSlots = 4;
        config.fxSlotsPerBus = 1;
        config.toneEffectPoolCapacity = 1;
        config.compressorEffectPoolCapacity = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto* engine = session.Engine();
        const std::array definitions = {
            MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f),
            MakeVariableDefinition(second, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f),
            MakeVariableDefinition(independent, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f)};
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harkentest::InstallDefinitionsWorld(engine, definitions.data(), 3, 0x5455504C).status);
        VnVoiceHandle render = 0;
        if (pair.effect == 0) {
            render = harkentest::RenderHandle(engine, PlayDc(engine, harkentest::ObjectId(9900), VN_BUS_MASTER));
            ASSERT_TRUE(engine->params.BindVoiceTarget(kVariable, render, pair.first,
                LinearCurve(0, pair.first0, 1, pair.first1)));
            ASSERT_TRUE(engine->params.BindVoiceTarget(second, render, pair.second,
                LinearCurve(0, pair.second0, 1, pair.second1)));
        } else {
            if (pair.effect == VN_EFFECT_THREE_BAND_TONE) {
                VnThreeBandToneDesc desc{};
                desc.structSize = sizeof(desc);
                desc.lowShelfFreqHz = 200;
                desc.peakFreqHz = 1000;
                desc.peakQ = 1;
                desc.highShelfFreqHz = 6000;
                ASSERT_EQ(VN_OK, Vn_SetBusEffect(VN_BUS_MASTER, 0, pair.effect,
                    reinterpret_cast<const VnEffectDesc*>(&desc)));
            } else {
                VnCompressorDesc desc{};
                desc.structSize = sizeof(desc);
                desc.thresholdDb = -18;
                desc.ratio = 4;
                desc.attackMs = 10;
                desc.releaseMs = 100;
                ASSERT_EQ(VN_OK, Vn_SetBusEffect(VN_BUS_MASTER, 0, pair.effect,
                    reinterpret_cast<const VnEffectDesc*>(&desc)));
            }
            const auto attach = [&](VnBindingId id, VnVariableId variable,
                                     std::uint32_t param, float from, float to) {
                RegisterLinearBinding(id, variable, VN_TARGET_SCOPE_BUS_EFFECT_PARAM,
                    harken::PackEffectParamLocator(pair.effect, 0, param), from, to);
                VnBindingAttachDesc desc{};
                desc.structSize = sizeof(desc);
                desc.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
                desc.objectId = VN_BUS_MASTER;
                ASSERT_EQ(VN_OK, Vn_AttachBinding(id, &desc));
            };
            attach(harkentest::ObjectId(9910), kVariable, pair.first, pair.first0, pair.first1);
            attach(harkentest::ObjectId(9911), second, pair.second, pair.second0, pair.second1);
        }
        RegisterLinearBinding(harkentest::ObjectId(9912), independent, VN_TARGET_SCOPE_BUS,
                              harken::kBusTargetBusVolume, 0, -6);
        VnBindingAttachDesc busAttach{};
        busAttach.structSize = sizeof(busAttach);
        busAttach.scopeKind = VN_TARGET_SCOPE_BUS;
        busAttach.objectId = VN_BUS_MASTER;
        ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(9912), &busAttach));
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(2);
        harken::VariableSliceRef blocker{};
        ASSERT_TRUE(engine->variableSlices.Reserve(
            engine->variables.CurrentWorldGeneration(), engine->variables.CurrentTransportEpoch(),
            0xD00D, 0, 0xD00D, 1, &blocker));
        if (pair.effect == 0) {
            ASSERT_TRUE(engine->params.TryClaim(render, pair.first, blocker,
                engine->params.TargetRevision(render, pair.first)));
        } else {
            ASSERT_TRUE(engine->effectParamTargets.TryClaim(
                harken::EffectParamTargetStore::Scope::Bus, 0, engine->buses.GenerationAt(0),
                0, pair.first, blocker,
                engine->effectParamTargets.TargetRevision(
                    harken::EffectParamTargetStore::Scope::Bus, 0, engine->buses.GenerationAt(0),
                    0, pair.first)));
        }
        const auto ticket = engine->logicCommits.IssuedCount() + 1;
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        ASSERT_EQ(VN_OK, Vn_SetValue(second, 1));
        ASSERT_EQ(VN_OK, Vn_SetValue(independent, 1));
        MakeAcceptedWriteDue(engine);
        EXPECT_FALSE(engine->variables.SliceForTicketForTests(ticket).Valid());
        const auto blocked = engine->variables.SliceForTicketForTests(ticket + 1);
        EXPECT_FALSE(blocked.Valid());
        EXPECT_EQ(3u, engine->variables.PendingCount());
        const auto separate = engine->variables.SliceForTicketForTests(ticket + 2);
        ASSERT_TRUE(separate.Valid());
        EXPECT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(separate));
        if (pair.effect == 0) engine->params.ReleaseClaim(render, pair.first, blocker);
        else engine->effectParamTargets.ReleaseClaim(
            harken::EffectParamTargetStore::Scope::Bus, 0, engine->buses.GenerationAt(0),
            0, pair.first, blocker);
        ASSERT_TRUE(engine->variableSlices.AbandonPrepared(blocker));
        for (std::uint32_t step = 0; step < 32 && engine->variables.PendingCount() != 0; ++step) {
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        EXPECT_EQ(0u, engine->variables.PendingCount())
            << "first slice=" << static_cast<int>(engine->variableSlices.State(
                engine->variables.SliceForTicketForTests(ticket)))
            << " second slice=" << static_cast<int>(engine->variableSlices.State(
                engine->variables.SliceForTicketForTests(ticket + 1)))
            << " first claimed=" << engine->effectParamTargets.ForeignClaim(
                harken::EffectParamTargetStore::Scope::Bus, 0, engine->buses.GenerationAt(0),
                0, pair.first)
            << " first resolved=" << engine->effectParamTargets.Reduce(
                harken::EffectParamTargetStore::Scope::Bus, 0, 0, pair.first)
            << " second resolved=" << engine->effectParamTargets.Reduce(
                harken::EffectParamTargetStore::Scope::Bus, 0, 0, pair.second);
        EXPECT_EQ(3u, engine->variables.AppliedCount());
    }
}

TEST(VariableCurrentSlice,
     CapturedTupleRejectsFailedSourceButIndependentWriteStillApplies) {
    const VnVariableId cutoff = harkentest::OffsetObjectId(kVariable, 1);
    const VnVariableId independent = harkentest::OffsetObjectId(kVariable, 2);
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    config.voiceBindingInstanceSlots = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
    const std::array definitions = {
        MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f),
        MakeVariableDefinition(cutoff, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f),
        MakeVariableDefinition(independent, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, definitions.data(), 3, 0x53524346).status);
    const auto render = harkentest::RenderHandle(engine, PlayDc(engine, harkentest::ObjectId(9950), VN_BUS_MASTER));
    ASSERT_TRUE(engine->params.BindVoiceTarget(kVariable, render, harken::kTargetVoiceLowpassQ,
        LinearCurve(0, 0, 1, 1)));
    ASSERT_TRUE(engine->params.BindVoiceTarget(cutoff, render, harken::kTargetVoiceLowpassCutoff,
        LinearCurve(0, 20000, 1, 1000)));
    RegisterLinearBinding(harkentest::ObjectId(9951), independent, VN_TARGET_SCOPE_BUS,
                          harken::kBusTargetBusVolume, 0, -6);
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(9951), &attach));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    const auto ticket = engine->logicCommits.IssuedCount() + 1;
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
    ASSERT_EQ(VN_OK, Vn_SetValue(cutoff, 1));
    ASSERT_EQ(VN_OK, Vn_SetValue(independent, 1));
    MakeAcceptedWriteDue(engine);
    const auto source = engine->variables.SliceForTicketForTests(ticket);
    const auto dependent = engine->variables.SliceForTicketForTests(ticket + 1);
    const auto separate = engine->variables.SliceForTicketForTests(ticket + 2);
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(source));
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(dependent));
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(separate));
    EXPECT_EQ(1u, engine->variableSlices.SourceCount(dependent));
    EXPECT_EQ(2u, engine->variableSlices.SourceObserverCount(source));
    EXPECT_EQ(0u, engine->variableSlices.SourceCount(separate));
    EXPECT_EQ(0u, engine->variableSlices.AppliedBlockSequence(dependent));
    ASSERT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim,
              engine->variableSlices.Invalidate(source, 1));
    engine->nullBackend.Pump(8);
    EXPECT_EQ(harken::VariableSliceState::Invalidated, engine->variableSlices.State(dependent));
    EXPECT_NE(0u, engine->variableSlices.AppliedBlockSequence(dependent));
    EXPECT_EQ(harken::kVariableSliceSourceRejected, engine->variableSlices.InvalidationReasons(dependent));
    EXPECT_EQ(harken::VariableSliceState::SliceApplied, engine->variableSlices.State(separate));
    EXPECT_FLOAT_EQ(20000, engine->params.LastCommitted(render, harken::kTargetVoiceLowpassCutoff));
    engine->variables.EndService(*engine);
    harken::VariableOutcomeRecord outcome{};
    bool sawDependent = false;
    while (capturedOutcomes.Pop(&outcome)) {
        if (outcome.acceptedTicket.sequence == ticket + 1) {
            sawDependent = true;
            EXPECT_EQ(harken::VariableOutcomeKind::TargetRejected, outcome.outcome);
        }
    }
    EXPECT_TRUE(sawDependent);
    EXPECT_EQ(1u, engine->variables.AppliedCount());
    EXPECT_EQ(0u, engine->variables.PendingCount());
}

TEST(VariableCurrentSlice,
     OverwrittenInputIsNotPinnedAndItsFailureDoesNotPoisonLatestCandidate) {
    const VnVariableId combined = harkentest::OffsetObjectId(kVariable, 1);
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.voiceBindingInstanceSlots = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const std::array definitions = {
        MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f),
        MakeVariableDefinition(combined, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, definitions.data(), 2, 0x5352434D).status);
    const auto render = harkentest::RenderHandle(engine, PlayDc(engine, harkentest::ObjectId(9960), VN_BUS_MASTER));
    ASSERT_TRUE(engine->params.BindVoiceTarget(kVariable, render, harken::kTargetVoiceVolume,
        LinearCurve(0, 0, 1, -6)));
    ASSERT_TRUE(engine->params.BindVoiceTarget(combined, render, harken::kTargetVoiceVolume,
        LinearCurve(0, 0, 1, -12)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    auto observed = harken::ObserveVariableCallAt(
        engine->variables.CurrentWorldGeneration(), engine->variables.CurrentTransportEpoch(),
        kBlockFrames, engine->timelinePool.WindowStartFrame(), engine->timelinePool.PublishCutoffFrame());
    observed.requestedEffectiveFrame = engine->mixer.BlocksRendered() * kBlockFrames;
    const auto accept = [&](VnVariableId variable, float value) {
        return engine->variables.AcceptGlobal(variable, value, observed,
            engine->params, engine->bindings, engine->busTargets);
    };
    const auto a = accept(kVariable, 0.25f);
    ASSERT_EQ(VN_OK, a.result);
    engine->variables.BeginService(*engine, engine->mixer.BlocksRendered(), kBlockFrames);
    engine->variables.PrepareClaimedSlices(*engine);
    const auto old = engine->variables.SliceForTicketForTests(a.ticket.sequence);
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(old));
    const auto b = accept(kVariable, 1);
    const auto c = accept(combined, 1);
    ASSERT_EQ(VN_OK, b.result);
    ASSERT_EQ(VN_OK, c.result);
    engine->variables.BeginService(*engine, engine->mixer.BlocksRendered(), kBlockFrames);
    engine->variables.PrepareClaimedSlices(*engine);
    const auto latest = engine->variables.SliceForTicketForTests(b.ticket.sequence);
    const auto dependent = engine->variables.SliceForTicketForTests(c.ticket.sequence);
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(latest));
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(dependent));
    EXPECT_EQ(1u, engine->variableSlices.SourceObserverCount(old));
    EXPECT_EQ(0u, engine->variableSlices.SourceCount(latest));
    EXPECT_EQ(2u, engine->variableSlices.SourceObserverCount(latest));
    EXPECT_EQ(1u, engine->variableSlices.SourceCount(dependent));
    ASSERT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim,
              engine->variableSlices.Invalidate(old, 1));
    engine->nullBackend.Pump(8);
    EXPECT_EQ(harken::VariableSliceState::SliceApplied, engine->variableSlices.State(latest));
    EXPECT_EQ(harken::VariableSliceState::SliceApplied, engine->variableSlices.State(dependent));
    engine->variables.EndService(*engine);
    EXPECT_EQ(2u, engine->variables.AppliedCount());
    EXPECT_NEAR(-18, engine->params.LastCommitted(render, harken::kTargetVoiceVolume), 1.0e-5f);
}

TEST(VariableCurrentSlice,
     ExactCensusOverflowRejectsWholeWriteWithoutChangingAnyTarget) {
    constexpr std::uint32_t kVoiceCount = 32;
    constexpr std::uint32_t kTargetsPerVoice = 8;
    constexpr std::uint32_t kVoiceTargetCount =
        kVoiceCount * kTargetsPerVoice;
    constexpr std::array<float, kTargetsPerVoice> initialValues = {
        0.0f, 0.0f, 20000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    constexpr std::array<float, kTargetsPerVoice> finalValues = {
        -6.0f, 100.0f, 1000.0f, 1000.0f,
        0.5f, 0.75f, 6.0f, 1.4f};

    VnConfig config = CurrentSliceConfig(kVoiceCount, kBlockFrames, kSampleRate);
    config.commandQueueDepth = 512;
    config.maxControlCommandsPerQuantum = 512;
    config.maxBindings = 2;
    config.maxBindingAttachments = 2;
    config.voiceBindingInstanceSlots = kTargetsPerVoice;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
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
    EXPECT_EQ(0u, engine->variables.PendingCount());
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

    engine->nullBackend.Pump(8);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(acceptedBefore + 1u, engine->variables.AcceptedCount());
    EXPECT_EQ(appliedBefore, engine->variables.AppliedCount());
    EXPECT_EQ(durableTicket, engine->logicCommits.IssuedCount());
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(0.0f, current);
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
    EXPECT_FLOAT_EQ(firstCommitted, engine->params.LastCommitted(
                                             renders.front(),
                                             harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(engine->params.ResolvedValue(
                        renders.front(), harken::kTargetVoiceVolume),
                    engine->params.LastCommitted(
                        renders.front(), harken::kTargetVoiceVolume));
    EXPECT_EQ(lastCommitted, engine->params.LastCommitted(
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
    ASSERT_TRUE(capturedOutcomes.Pop(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::TargetRejected, outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(0u, outcome.bankVersionStableOrdinal);

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(durableTicket, engine->logicCommits.IssuedCount());
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
}

TEST(VariableCurrentSlice,
     ClaimFailureReleasesOwnedPrefixAndRetriesTheSameDurableWrite) {
    constexpr VnBusId kSecondBus = harkentest::ObjectId(9);
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.maxBuses = 4;
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
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
    ASSERT_TRUE(capturedOutcomes.Pop(&outcome));
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
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

TEST(VariableCurrentSlice, MixerConsumesProofBeyondRevisionStorageAndReclaimsAfterFence) {
    auto config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    // Meter-probe headroom must leave apply payloads beyond revision storage.
    config.maxBuses = 8;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    engine->nullBackend.Pump(1);
    const auto frame = engine->timelinePool.PublishCutoffFrame();
    const auto revisionSlots = engine->variables.RevisionStorageCapacity();
    ASSERT_GT(revisionSlots, 4096u);
    ASSERT_GT(engine->variableSlices.PayloadCapacity(), revisionSlots);
    std::vector<harken::VariableSliceRef> blockers(revisionSlots);
    for (std::uint32_t i = 0; i < revisionSlots; ++i)
        ASSERT_TRUE(engine->variableSlices.Reserve(1, 1, i + 100u, frame, i + 1u, 0, &blockers[i]));
    const auto target = BusVolumeSliceTarget(engine, -12.0f);
    harken::VariableSliceRef high{};
    ASSERT_TRUE(FileCurrentSlice(engine, frame, 1, &target, 1, &high));
    ASSERT_GE(high.slot, revisionSlots);
    engine->timelinePool.Publish(frame + kBlockFrames);
    for (unsigned quantum = 0; quantum < 8 &&
        engine->variableSlices.State(high) != harken::VariableSliceState::SliceApplied; ++quantum)
        engine->nullBackend.Pump(1);
    ASSERT_EQ(harken::VariableSliceState::SliceApplied, engine->variableSlices.State(high));
    ASSERT_NE(nullptr, engine->buses.BusState(0));
    EXPECT_FLOAT_EQ(DbToLinear(-12.0f), engine->buses.BusState(0)->gain.target);
    const auto applied = engine->variableSlices.AppliedBlockSequence(high);
    ASSERT_TRUE(engine->variableSlices.Acknowledge(high));
    engine->variableSlices.ReclaimSettled(applied);
    EXPECT_EQ(harken::VariableSliceState::SliceApplied, engine->variableSlices.State(high));
    engine->nullBackend.Pump(1);
    engine->variableSlices.ReclaimSettled(engine->mixer.BlocksRendered());
    EXPECT_EQ(harken::VariableSliceState::Free, engine->variableSlices.State(high));
    for (const auto blocker : blockers) EXPECT_TRUE(engine->variableSlices.AbandonPrepared(blocker));
    EXPECT_EQ(engine->variableSlices.Capacity(), engine->variableSlices.FreeRowCount());
    EXPECT_EQ(engine->variableSlices.PayloadCapacity(), engine->variableSlices.FreePayloadCount());
    EXPECT_EQ(engine->variableSlices.TargetPoolCapacity(), engine->variableSlices.FreeTargetCapacity());
}

TEST(VariableCurrentSlice, PublishedEmptyPageAdvancesTheFixedPrefix) {
    EngineSession session(CurrentSliceConfig(1, kBlockFrames, kSampleRate));
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
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
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
    const auto publicConfig = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
        const auto maximum = engine->timelinePool.MaxVariableCurrentSlicesPerQuantum();
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
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
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
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
        const auto accepted = engine->variables.AcceptGlobal(kVariable, value,
            observation, engine->params, engine->bindings, engine->busTargets);
        ASSERT_EQ(VN_OK, accepted.result);
        engine->variables.BeginService(*engine,
                                       engine->mixer.BlocksRendered(),
                                       kBlockFrames);
        engine->variables.EndService(*engine);
        EXPECT_EQ(freeRows - 1u, engine->variableSlices.FreeRowCount());
        EXPECT_EQ(freeTargets,
                  engine->variableSlices.FreeTargetCapacity());
        EXPECT_EQ(1u, engine->variables.PendingCount());
        const auto slice = engine->variables.SliceForTicketForTests(accepted.ticket.sequence);
        ASSERT_TRUE(slice.Valid());
        EXPECT_EQ(0u, engine->variableSlices.TargetCount(slice));
        for (std::uint32_t step = 0; step < 16 && engine->variables.PendingCount() != 0; ++step) {
            engine->nullBackend.Pump(1);
            engine->variables.PrepareClaimedSlices(*engine);
            engine->variables.EndService(*engine);
        }
        EXPECT_EQ(0u, engine->variables.PendingCount());
        float current = 0.0f;
        ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
        EXPECT_FLOAT_EQ(value, current);
        engine->nullBackend.Pump(1);
        engine->variables.EndService(*engine);
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

TEST(VariableCurrentSlice, NoConsumerRequiresPublishedSliceAndConsumptionFence) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
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
    EXPECT_EQ(1u, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->variables.AppliedCount());
    for (std::uint32_t step = 0; step < 16 && engine->variables.PendingCount() != 0; ++step) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(1u, engine->variables.AppliedCount());
}

TEST(VariableCurrentSlice, LaterUnrelatedCommandsDoNotRaiseCapturedFrontier) {
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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

TEST(VariableCurrentSlice, NewPhysicalTargetWaitsForInstallationAfterBankOnlyPreparation) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    InstallVariable(engine);
    const auto sound = harkentest::ObjectId(7391);
    const std::vector<float> samples(kSampleRate, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    auto observed = harken::ObserveVariableCallAt(engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    const auto accepted = engine->variables.AcceptGlobal(kVariable, 1.0f, observed,
        engine->params, engine->bindings, engine->busTargets);
    ASSERT_EQ(VN_OK, accepted.result);
    engine->variables.BeginService(*engine, engine->mixer.BlocksRendered(), kBlockFrames);
    const auto bankOnly = engine->variables.SliceForTicketForTests(accepted.ticket.sequence);
    ASSERT_TRUE(bankOnly.Valid());
    ASSERT_EQ(0u, engine->variableSlices.TargetCount(bankOnly));

    auto play = MakePlayDesc(sound);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const auto render = harkentest::RenderHandle(engine, voice);
    ASSERT_TRUE(engine->params.BindVoiceTarget(kVariable, render,
        harken::kTargetVoiceVolume, LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_LT(engine->commands.SettledFrontier(), engine->commands.PublishedHead());
    engine->mixer.SetMaxCommandsPerQuantumForTest(0);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto physical = engine->variables.SliceForTicketForTests(accepted.ticket.sequence);
    ASSERT_TRUE(physical.Valid());
    ASSERT_EQ(1u, engine->variableSlices.TargetCount(physical));
    engine->nullBackend.Pump(8);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->variables.AppliedCount());
    EXPECT_EQ(1u, engine->variables.PendingCount());
    float value = -1.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &value));
    EXPECT_FLOAT_EQ(0.0f, value);

    engine->mixer.SetMaxCommandsPerQuantumForTest(UINT32_MAX);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    EXPECT_EQ(1u, engine->variables.AppliedCount());
    EXPECT_EQ(0u, engine->variables.PendingCount());
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &value));
    EXPECT_FLOAT_EQ(1.0f, value);
    EXPECT_FLOAT_EQ(-12.0f, engine->params.ResolvedValue(render, harken::kTargetVoiceVolume));
}

TEST(VariableCurrentSlice, EmitterLifecycleResolvesAroundAudioClaim) {
    {
        VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
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
        VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
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
    while (capturedOutcomes.Pop(&discarded)) {}
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
    ASSERT_TRUE(capturedOutcomes.Pop(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_NE(0u, outcome.bankVersionStableOrdinal);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(freeRows, engine->variableSlices.FreeRowCount());
    EXPECT_EQ(freeTargets, engine->variableSlices.FreeTargetCapacity());
}

TEST(VariableCurrentSlice, RejectedEmitterSetDoesNotActivateItsScopeBank) {
    VnConfig config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
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
        engine->variables.EarliestReadCutFrame(*engine) + kBlockFrames;
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
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto setSlice = engine->variables.SliceForTicketForTests(set.ticket.sequence);
    const auto globalSlice = engine->variables.SliceForTicketForTests(globalWrite.ticket.sequence);
    ASSERT_TRUE(setSlice.Valid());
    ASSERT_TRUE(globalSlice.Valid());
    EXPECT_EQ(setObservation.requestedEffectiveFrame,
        engine->variableSlices.EffectiveFrame(setSlice));
    EXPECT_EQ(globalObservation.requestedEffectiveFrame,
        engine->variableSlices.EffectiveFrame(globalSlice));
    EXPECT_TRUE(engine->variables.HasPendingWriteForVoiceTarget(
        engine->params, render, harken::kTargetVoiceVolume));

    bool sawSetOutcome = false;
    bool sawGlobalOutcome = false;
    for (std::uint32_t block = 0;
         block < 64 && engine->variables.PendingCount() != 0; ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        harken::VariableOutcomeRecord outcome{};
        while (capturedOutcomes.Pop(&outcome)) {
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
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
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
    ASSERT_TRUE(capturedOutcomes.Pop(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::EmitterDestroyed,
              outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(0u, outcome.relatedTicket.sequence);
    EXPECT_EQ(expectedActualFrame, outcome.actualFrame);
    EXPECT_EQ(0u, outcome.bankVersionStableOrdinal);
    EXPECT_FALSE(capturedOutcomes.Pop(&outcome));
    EXPECT_FLOAT_EQ(1.0f,
                    engine->voices.Resolve(firstRender)->gain.target);
    EXPECT_FLOAT_EQ(1.0f,
                    engine->voices.Resolve(secondRender)->gain.target);
}

TEST(VariableCurrentSlice,
     EmitterDestroyBeforeAudioClaimWaitsForTheStrictTombstoneFence) {
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
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
    ASSERT_TRUE(capturedOutcomes.Pop(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::EmitterDestroyed,
              outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(0u, outcome.relatedTicket.sequence);
    EXPECT_EQ(expectedActualFrame, outcome.actualFrame);
    EXPECT_EQ(0u, outcome.bankVersionStableOrdinal);
    EXPECT_FALSE(capturedOutcomes.Pop(&outcome));

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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
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
    ASSERT_TRUE(capturedOutcomes.Pop(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::EmitterDestroyed,
              outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(tombstoneBlock * kBlockFrames, outcome.actualFrame);
    EXPECT_LT(outcome.actualFrame,
              engine->mixer.BlocksRendered() * kBlockFrames);
}

TEST(VariableCurrentSlice,
     EmitterDestroyAtAudioClaimAppliesWholeThenFallsBackToGlobal) {
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    config.maxBindings = 4;
    config.voiceBindingInstanceSlots = 2;
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
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
    ASSERT_TRUE(capturedOutcomes.Pop(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::EmitterDestroyed,
              outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(0u, outcome.relatedTicket.sequence);
    EXPECT_EQ(appliedBlock * kBlockFrames, outcome.actualFrame);
    EXPECT_EQ(0u, outcome.bankVersionStableOrdinal);
    EXPECT_FALSE(capturedOutcomes.Pop(&outcome));

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
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
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
    ASSERT_TRUE(capturedOutcomes.Pop(&outcome));
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
    EXPECT_EQ(durableTicket, outcome.acceptedTicket.sequence);
    EXPECT_EQ(0u, outcome.relatedTicket.sequence);
    EXPECT_EQ(appliedBlock * kBlockFrames, outcome.actualFrame);
    EXPECT_EQ(1u, outcome.bankVersionStableOrdinal);
    EXPECT_FALSE(capturedOutcomes.Pop(&outcome));

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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
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
        EngineSession session(CurrentSliceConfig(1, kBlockFrames, kSampleRate));
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
        VnConfig config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
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
        VnConfig config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
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

TEST(VariableCurrentSlice, PlayInBankAckGapUsesCapturedValueForFirstPcm) {
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallVariable(engine);
    const VnVoiceHandle existing = PlayDc(engine, harkentest::ObjectId(1), VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Vn_SetVoicePan(existing, -1));
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
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_NE(0u, voice);
    ASSERT_EQ(VN_OK, Vn_SetVoicePan(voice, 1));
    ASSERT_EQ(VN_OK, Vn_Update());
    bool heard = false;
    for (std::uint32_t block = 0; block < 16 && !heard; ++block) {
        engine->nullBackend.Pump(1);
        const auto* output = engine->nullBackend.LastBlock();
        if (std::abs(output[2 * (kBlockFrames - 1) + 1]) < 1e-7f) continue;
        heard = true;
        for (std::uint32_t frame = kBlockFrames / 2; frame < kBlockFrames; ++frame)
            EXPECT_NEAR(0.05f * DbToLinear(-12) * std::sqrt(2.0f), output[2 * frame + 1], 1e-6f);
    }
    EXPECT_TRUE(heard);
}

TEST(VariableCurrentSlice, OverdueBankOnlyWritesPublishTogetherAndSettleInFrameOrder) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
    InstallVariable(engine);

    harken::VariableCallObservation first = harken::ObserveVariableCallAt(
        engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    first.requestedEffectiveFrame = 4 * kBlockFrames;
    harken::VariableCallObservation second = first;
    second.requestedEffectiveFrame = 5 * kBlockFrames;
    const auto a = engine->variables.AcceptGlobal(
                         kVariable, 0.25f, first, engine->params,
                         engine->bindings, engine->busTargets);
    const auto b = engine->variables.AcceptGlobal(
                         kVariable, 0.75f, second, engine->params,
                         engine->bindings, engine->busTargets);
    ASSERT_EQ(VN_OK, a.result);
    ASSERT_EQ(VN_OK, b.result);

    engine->nullBackend.Pump(6);
    engine->variables.BeginService(*engine, engine->mixer.BlocksRendered(), kBlockFrames);
    engine->variables.PrepareClaimedSlices(*engine);
    const auto aRef = engine->variables.SliceForTicketForTests(a.ticket.sequence);
    const auto bRef = engine->variables.SliceForTicketForTests(b.ticket.sequence);
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(aRef));
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine->variableSlices.State(bRef));
    EXPECT_EQ(0u, engine->variableSlices.TargetCount(aRef));
    EXPECT_EQ(0u, engine->variableSlices.TargetCount(bRef));
    engine->variables.EndService(*engine);
    EXPECT_EQ(0u, engine->variables.AppliedCount());
    EXPECT_EQ(2u, engine->variables.PendingCount());
    float current = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(0.0f, current);

    engine->nullBackend.Pump(8);
    engine->variables.EndService(*engine);
    EXPECT_EQ(2u, engine->variables.AppliedCount());
    EXPECT_EQ(0u, engine->variables.PendingCount());
    ASSERT_TRUE(engine->variables.CurrentValue(kVariable, &current));
    EXPECT_FLOAT_EQ(0.75f, current);
    harken::VariableOutcomeRecord outcome{};
    ASSERT_TRUE(capturedOutcomes.Pop(&outcome));
    EXPECT_EQ(a.ticket.sequence, outcome.acceptedTicket.sequence);
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
    const auto firstActualFrame = outcome.actualFrame;
    ASSERT_TRUE(capturedOutcomes.Pop(&outcome));
    EXPECT_EQ(b.ticket.sequence, outcome.acceptedTicket.sequence);
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
    EXPECT_LE(firstActualFrame, outcome.actualFrame);
}

TEST(VariableCurrentSlice,
     GlobalAndEmitterSetHonorTicketOrderAfterServiceCursorRotation) {
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harkentest::VariableOutcomeCapture capturedOutcomes(*engine);
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
            capturedOutcomes.Pop(&firstOutcome)) {
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
        VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
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
        const auto firstWrite = globalFirst ? acceptGlobal() : acceptLocal();
        const auto secondWrite = globalFirst ? acceptLocal() : acceptGlobal();
        ASSERT_EQ(VN_OK, firstWrite.result);
        ASSERT_EQ(VN_OK, secondWrite.result);

        const std::uint64_t appliedBefore = engine->variables.AppliedCount();
        MakeAcceptedWriteDue(engine);
        const auto firstSlice = engine->variables.SliceForTicketForTests(firstWrite.ticket.sequence);
        const auto secondSlice = engine->variables.SliceForTicketForTests(secondWrite.ticket.sequence);
        const auto& slices = std::as_const(engine->variableSlices);
        ASSERT_EQ(harken::VariableSliceState::PageCommitted, slices.State(firstSlice));
        ASSERT_EQ(harken::VariableSliceState::PageCommitted, slices.State(secondSlice));
        const auto snapshotFor = [&](harken::VariableSliceRef slice, VnVoiceHandle render) {
            for (std::uint32_t i = 0; i < slices.TargetCount(slice); ++i) {
                const auto* target = slices.Target(slice, i);
                if (target->locator.object == render) return target->resolvedValue;
            }
            ADD_FAILURE() << "Missing voice snapshot";
            return 100.0f;
        };
        const float firstDb = 20.0f * std::log10(globalFirst ? globalGain : localGain);
        EXPECT_NEAR(firstDb, snapshotFor(firstSlice, clearedRender), 1.0e-5f);
        EXPECT_NEAR(clearedDb, snapshotFor(secondSlice, clearedRender), 1.0e-5f);
        for (std::uint32_t step = 0;
             step < 32 && engine->variables.PendingCount() != 0;
             ++step) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
        }
        ASSERT_EQ(appliedBefore + 2, engine->variables.AppliedCount());
        ASSERT_EQ(0u, engine->variables.PendingCount());

        float global = 0.0f;
        float cleared = 0.0f;
        float retained = 0.0f;
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
     OrderedServiceReachesIndependentBankOnlyKeyUnderTargetPressure) {
    constexpr std::uint32_t kTargetKeys = 32;
    constexpr std::uint32_t kKeyCount = kTargetKeys + 1;
    constexpr VnVariableId kFirstVariable = harkentest::OffsetObjectId(kVariable, 100);
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.maxBindings = kTargetKeys;
    config.maxBindingAttachments = kTargetKeys;
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
    for (std::uint32_t i = 0; i < kTargetKeys; ++i) {
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
    engine->variables.PrepareClaimedSlices(*engine);
    engine->variables.EndService(*engine);
    for (std::uint32_t step = 0; step < 16 && engine->variables.AppliedCount() == 0; ++step) {
        engine->nullBackend.Pump(1);
        engine->variables.EndService(*engine);
    }
    EXPECT_EQ(kTargetKeys,
              engine->variables.PendingCount());
    EXPECT_EQ(1u, engine->variables.AppliedCount());
    float current = -1.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(
        harkentest::OffsetObjectId(kFirstVariable, kTargetKeys), &current));
    EXPECT_FLOAT_EQ(0.5f, current);
    for (std::uint32_t i = 0; i < kTargetKeys; ++i) {
        ASSERT_TRUE(engine->variables.CurrentValue(harkentest::OffsetObjectId(kFirstVariable, i),
                                                  &current));
        EXPECT_FLOAT_EQ(0.0f, current);
    }
    for (harken::VariableSliceRef ref : blockers) {
        ASSERT_TRUE(engine->variableSlices.AbandonPrepared(ref));
    }
}

TEST(VariableCurrentSlice, PublicationRefusesPhysicalAndUnreclaimedSlicesWithoutDraining) {
    EngineSession session(CurrentSliceConfig(4, kBlockFrames, kSampleRate));
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

TEST(VariableCurrentSlice, PublicationWaitsForBankOnlySliceAndResetsBankGenerations) {
    EngineSession session(CurrentSliceConfig(4, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const VnVariableWriteV1 write{kVariable, 0.5f, 0};
    VnVariableBatchDescV1 batch{};
    batch.structSize = sizeof(batch);
    batch.writeCount = 1;
    batch.writes = &write;
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&batch, &ticket));
    e.nullBackend.Pump(8);
    e.variables.BeginService(e, e.mixer.BlocksRendered(), kBlockFrames);
    e.variables.PrepareClaimedSlices(e);
    const auto slice = e.variables.SliceForTicketForTests(ticket.sequence);
    harken::VariableSliceSource source{};
    ASSERT_TRUE(e.variableSlices.CaptureSource(slice, &source));
    EXPECT_EQ(ticket.sequence, source.ticket);
    EXPECT_EQ(ticket.worldGeneration, source.world);
    EXPECT_EQ(ticket.transportGeneration, source.transport);
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(slice));
    EXPECT_EQ(0u, e.variableSlices.AppliedBlockSequence(slice));
    const auto effectiveFrame = e.variableSlices.EffectiveFrame(slice);
    const auto expectTicket = [&](std::uint32_t outcome) {
        VnVariableTicketQueryDescV1 query{};
        query.structSize = sizeof(query);
        query.ticket = ticket;
        VnVariableTicketQueryResultV1 result{};
        result.structSize = sizeof(result);
        ASSERT_EQ(VN_OK, Vn_QueryVariableTicket(&query, &result));
        EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, result.status);
        EXPECT_EQ(outcome, result.outcome);
    };
    const auto definition = *e.variables.Definition(kVariable);
    harken::VariableServiceV1::PreparedPackDefinitions token;
    ASSERT_EQ(VN_BACKPRESSURE, e.variables.PreparePackDefinitions(&definition, 1,
        harken::PackVariablePublicationReason::InitPublication, 777, &token));
    EXPECT_EQ(0u, e.variables.AppliedCount());
    ASSERT_NO_FATAL_FAILURE(expectTicket(VN_VARIABLE_OUTCOME_PENDING));
    e.nullBackend.Pump(8);
    ASSERT_EQ(harken::VariableSliceState::SliceApplied, e.variableSlices.State(slice));
    const auto appliedSequence = e.variableSlices.AppliedBlockSequence(slice);
    EXPECT_EQ(effectiveFrame / kBlockFrames + 1u, appliedSequence);
    EXPECT_LE(appliedSequence, e.mixer.BlocksRendered());
    e.variables.EndService(e);
    ASSERT_EQ(1u, e.variables.AppliedCount());
    ASSERT_NO_FATAL_FAILURE(expectTicket(VN_VARIABLE_OUTCOME_APPLIED));
    e.nullBackend.Pump(1);
    e.variables.EndService(e);
    harken::detail::VariableServiceTestAccess::SeedGlobalBankVersionGeneration(e.variables, UINT32_MAX);
    ASSERT_TRUE(harken::detail::VariableServiceTestAccess::SeedEmitterBankVersionGeneration(e.variables, emitter, UINT32_MAX));
    ASSERT_EQ(VN_OK, e.variables.PreparePackDefinitions(&definition, 1,
        harken::PackVariablePublicationReason::InitPublication, 777, &token));
    e.variables.CommitPreparedPackDefinitions(&token);
    EXPECT_EQ((std::array<std::uint32_t, 2>{1, 1}),
              harken::detail::VariableServiceTestAccess::BankGenerations(e.variables, emitter));
    harken::VariableOutcomeRecord record{};
    ASSERT_TRUE(e.variables.PopOutcome(&record));
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, record.outcome);
    EXPECT_EQ(ticket.sequence, record.acceptedTicket.sequence);
    EXPECT_EQ(appliedSequence * kBlockFrames, record.actualFrame);
    ASSERT_NO_FATAL_FAILURE(expectTicket(VN_VARIABLE_OUTCOME_APPLIED));
    e.variables.EndService(e);
    EXPECT_EQ(1u, e.variables.AppliedCount());
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.25f));
}

TEST(VariableCurrentSlice, PublicationRefusesPreparedRowsInvalidHistoryAndExhaustedWorld) {
    EngineSession session(CurrentSliceConfig(4, kBlockFrames, kSampleRate));
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

TEST(VariableCurrentSlice, PublicationTerminatesFullRevisionBudgetWithBoundedOutcomeStorage) {
    using Access = harken::detail::VariableServiceTestAccess;
    EngineSession session(CurrentSliceConfig(4, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto firstTicket = e.logicCommits.IssuedCount() + 1;
    const void* storage = Access::OutcomeStorage(e.variables);
    const auto capacity = e.variables.PendingCapacity();
    const auto physicalCapacity = e.variables.RevisionStorageCapacity();
    std::vector<bool> seen(capacity, false);
    harken::VariableOutcomeRecord record{};
    for (std::size_t i = 0; i < capacity; ++i) {
        auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
            e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
        observed.requestedEffectiveFrame = (i + 1) * kBlockFrames;
        ASSERT_EQ(VN_OK, e.variables.AcceptGlobal(kVariable, 0.5f, observed,
            e.params, e.bindings, e.busTargets).result);
    }
    ASSERT_EQ((std::array<std::size_t, 3>{0, physicalCapacity, 8}), Access::OutcomeCapacity(e.variables));
    const auto definition = *e.variables.Definition(kVariable);
    const auto world = e.variables.CurrentWorldGeneration();
    harken::VariableServiceV1::PreparedPackDefinitions token;
    ASSERT_EQ(VN_OK, e.variables.PreparePackDefinitions(&definition, 1,
        harken::PackVariablePublicationReason::InitPublication, 987, &token));
    e.variables.CommitPreparedPackDefinitions(&token);
    EXPECT_EQ(storage, Access::OutcomeStorage(e.variables));
    EXPECT_EQ((std::array<std::size_t, 3>{capacity, physicalCapacity, 8}), Access::OutcomeCapacity(e.variables));
    EXPECT_EQ(capacity, e.variables.TerminationCount());
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
    EXPECT_EQ(capacity, terminations);
    EXPECT_TRUE(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
    e.variables.MarkOutcomeDeliveryComplete();
    e.variables.FinishOutcomeDelivery();
    EXPECT_EQ((std::array<std::size_t, 3>{0, physicalCapacity, physicalCapacity}), Access::OutcomeCapacity(e.variables));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.25f));
    EXPECT_EQ(storage, Access::OutcomeStorage(e.variables));
}

TEST(VariableCurrentSlice, WorldReplacementDrainsPhysicalCutAndClosesPlay) {
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(3, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(3, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
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
    EXPECT_EQ(1u, engine->paramDropCount);
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
    EXPECT_EQ(1u, engine->paramDropCount);
}

TEST(VariableCurrentSlice, RejectedVolumeCandidatesCountEachPhysicalTargetOnce) {
    EngineSession session(CurrentSliceConfig(4, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    InstallVariable(engine);
    const auto bus = harkentest::ObjectId(9);
    ASSERT_EQ(VN_OK, Vn_CreateBus(bus, VN_BUS_MASTER));
    const auto voice = PlayDc(engine, harkentest::ObjectId(1), bus);
    const auto render = harkentest::RenderHandle(engine, voice);
    for (std::uint32_t binding = 0; binding < 2; ++binding)
        ASSERT_TRUE(engine->params.BindVoiceTarget(kVariable, render,
            harken::kTargetVoiceVolume, LinearCurve(0, 0, 1, 772)));
    RegisterBusVolumeBinding(harkentest::ObjectId(1), bus, 0, 0, 1, 772);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    const auto dropsBefore = engine->paramDropCount;
    const auto ticket = AcceptGlobalWrite(kVariable, 1);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    EXPECT_EQ(VN_VARIABLE_OUTCOME_REJECTED_TARGET, QueryTicket(ticket).outcome);
    EXPECT_EQ(dropsBefore + 2, engine->paramDropCount);
    EXPECT_FLOAT_EQ(0, QueryCurrentValue(kVariable));
    EXPECT_FLOAT_EQ(1, engine->voices.Resolve(render)->gain.target);
    EXPECT_FLOAT_EQ(1, engine->buses.BusState(engine->buses.FindIndex(bus))->gain.target);
    for (std::uint32_t step = 0; step < 4; ++step) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(dropsBefore + 2, engine->paramDropCount);
    }
    const auto legal = AcceptGlobalWrite(kVariable, 0.1f);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    EXPECT_EQ(VN_VARIABLE_OUTCOME_APPLIED, QueryTicket(legal).outcome);
    EXPECT_EQ(dropsBefore + 2, engine->paramDropCount);
}

TEST(VariableCurrentSlice, RejectedGlobalBatchRestoresAllEmitterOverridesAndProjections) {
    EngineSession session(CurrentSliceConfig(4, kBlockFrames, kSampleRate));
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
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(engine->variables.RevisionStorageCapacity(),
        harken::detail::VariableServiceTestAccess::OutcomeCapacity(engine->variables)[2]);
}

TEST(VariableCurrentSlice, FiniteOverflowBusWriteRowIsTargetRejected) {
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
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
    VnConfig config = CurrentSliceConfig(4, kBlockFrames, kSampleRate);
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

TEST(VariableCurrentSlice, ReadCutPreparationLeavesSixtyThreeUnrelatedVariablesPending) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    std::array<harken::VariableDefinitionV1, 64> definitions{};
    std::array<harken::VariableAcceptResult, 64> writes{};
    for (std::uint32_t i = 0; i < definitions.size(); ++i)
        definitions[i] = MakeVariableDefinition(harkentest::OffsetObjectId(kVariable, i),
            harken::VariableKindV1::Smooth, 0, 1, 0);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, definitions.data(), definitions.size()).status);
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        writes[i] = e.variables.AcceptGlobal(definitions[i].variableId, 0.75f,
            observed, e.params, e.bindings, e.busTargets);
        ASSERT_EQ(VN_OK, writes[i].result);
    }
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames,
        writes.back().ticket.sequence + 1, 0, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, definitions.back().variableId, &value));
    ASSERT_FLOAT_EQ(0.75f, value);
    ASSERT_TRUE(e.variables.PrepareReadCutSources(e, cut));
    const auto source = e.variables.SliceForTicketForTests(writes.back().ticket.sequence);
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(source));
    for (std::uint32_t i = 0; i + 1 < writes.size(); ++i)
        EXPECT_FALSE(e.variables.SliceForTicketForTests(writes[i].ticket.sequence).Valid());
    EXPECT_FALSE(harken::detail::VariableServiceTestAccess::HasHeldPlacements(e.variables));
    EXPECT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    e.variables.PublishPreparedPlaybackPages();
    e.nullBackend.Pump(8);
    e.variables.EndService(e);
    EXPECT_EQ(1u, e.variables.AppliedCount());
    EXPECT_EQ(63u, e.variables.PendingCount());
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, ReadCutPreparationClosesOverPhysicalFilterPredecessorOnly) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    const auto q = harkentest::OffsetObjectId(kVariable, 1);
    const auto unrelated = harkentest::OffsetObjectId(kVariable, 2);
    const std::array definitions = {
        MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0, 1, 0),
        MakeVariableDefinition(q, harken::VariableKindV1::Smooth, 0, 1, 0),
        MakeVariableDefinition(unrelated, harken::VariableKindV1::Smooth, 0, 1, 0)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, definitions.data(), definitions.size()).status);
    const auto render = harkentest::RenderHandle(&e, PlayDc(&e, harkentest::ObjectId(9971), VN_BUS_MASTER));
    ASSERT_TRUE(e.params.BindVoiceTarget(kVariable, render, harken::kTargetVoiceLowpassCutoff,
        LinearCurve(0, 20000, 1, 1000)));
    ASSERT_TRUE(e.params.BindVoiceTarget(q, render, harken::kTargetVoiceLowpassQ, LinearCurve(0, 0, 1, 1)));
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(4);
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, e.timelinePool.WindowStartFrame(),
        e.timelinePool.PublishCutoffFrame());
    const auto ignored = e.variables.AcceptGlobal(unrelated, 1, observed, e.params, e.bindings, e.busTargets);
    const auto prior = e.variables.AcceptGlobal(kVariable, 1, observed, e.params, e.bindings, e.busTargets);
    const auto root = e.variables.AcceptGlobal(q, 1, observed, e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, ignored.result);
    ASSERT_EQ(VN_OK, prior.result);
    ASSERT_EQ(VN_OK, root.result);
    const auto frame = e.variables.EarliestReadCutFrame(e);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, frame, root.ticket.sequence + 1, 0, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, q, &value));
    ASSERT_TRUE(e.variables.PrepareReadCutSources(e, cut));
    const auto source = e.variables.SliceForTicketForTests(prior.ticket.sequence);
    const auto dependent = e.variables.SliceForTicketForTests(root.ticket.sequence);
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(source));
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(dependent));
    EXPECT_FALSE(e.variables.SliceForTicketForTests(ignored.ticket.sequence).Valid());
    EXPECT_EQ(1u, e.variableSlices.SourceCount(dependent));
    const auto& slices = std::as_const(e.variableSlices);
    ASSERT_NE(nullptr, slices.Target(dependent, 0));
    EXPECT_FLOAT_EQ(1000, slices.Target(dependent, 0)->command.payload.setVoiceFilter.cutoffHz);
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, OrdinaryTargetDebtProtectsRecoveredSpanFromRepeatedReadCuts) {
    for (unsigned variant = 0; variant < 6; ++variant) {
        SCOPED_TRACE(variant);
        EngineSession session(CurrentSliceConfig(8, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        const auto small = harkentest::OffsetObjectId(kVariable, 1);
        const std::array definitions = {
            MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0, 1, 0),
            MakeVariableDefinition(small, harken::VariableKindV1::Smooth, 0, 1, 0)};
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
            harkentest::InstallDefinitionsWorld(&e, definitions.data(), definitions.size()).status);
        VnVoiceHandle firstRender = 0;
        for (std::uint32_t i = 0; i < 5; ++i) {
            const auto render = harkentest::RenderHandle(&e,
                PlayDc(&e, harkentest::ObjectId(9950 + i), VN_BUS_MASTER));
            if (i == 0) firstRender = render;
            ASSERT_TRUE(e.params.BindVoiceTarget(i < 4 ? kVariable : small, render,
                harken::kTargetVoiceVolume, LinearCurve(0, 0, 1, -6)));
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(4);
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(2);
        ASSERT_TRUE(e.variableSlices.Reset(32, 4, 8, 32));
        std::array<harken::VariableSliceRef, 4> blockers{};
        for (std::uint32_t i = 0; i < blockers.size(); ++i)
            ASSERT_TRUE(e.variableSlices.Reserve(999, 1, i + 1, 0, i + 1, 2, &blockers[i]));
        ASSERT_TRUE(e.variableSlices.AbandonPrepared(blockers[0]));
        ASSERT_TRUE(e.variableSlices.AbandonPrepared(blockers[2]));
        ASSERT_EQ(4u, e.variableSlices.FreeTargetCapacity());
        ASSERT_EQ(harken::VariableSliceTable::ReservationStatus::TargetSpan,
            e.variableSlices.ReservationAvailability(4));
        harken::VariableSliceRef foreign{};
        if (variant == 1) {
            ASSERT_TRUE(e.variableSlices.Reserve(999, 1, 10, 0, 10, 0, &foreign));
            ASSERT_TRUE(e.params.TryClaim(firstRender, harken::kTargetVoiceVolume, foreign,
                e.params.TargetRevision(firstRender, harken::kTargetVoiceVolume)));
        }
        auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
            e.variables.CurrentTransportEpoch(), kBlockFrames, e.timelinePool.WindowStartFrame(),
            e.timelinePool.PublishCutoffFrame());
        const auto ordinary = e.variables.AcceptGlobal(kVariable, 1, observed,
            e.params, e.bindings, e.busTargets);
        ASSERT_EQ(VN_OK, ordinary.result);
        const auto publishUnrelatedPlay = [&] {
            const auto play = MakePlayDesc(harkentest::ObjectId(9950));
            VnVoiceHandle voice = 0;
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
            ASSERT_LT(e.commands.SettledFrontier(), e.commands.PublishedHead());
        };
        if (variant == 4) publishUnrelatedPlay();
        e.variables.BeginService(e, e.mixer.BlocksRendered(), kBlockFrames);
        e.variables.PrepareClaimedSlices(e);
        EXPECT_FALSE(e.variables.SliceForTicketForTests(ordinary.ticket.sequence).Valid());
        ASSERT_FALSE(harken::detail::VariableServiceTestAccess::HasHeldPlacements(e.variables));
        if (variant == 4) {
            e.nullBackend.Pump(1);
            publishUnrelatedPlay();
        }
        if (variant == 5) {
            ASSERT_TRUE(e.variableSlices.Reserve(999, 1, 10, 0, 10, 0, &foreign));
            ASSERT_TRUE(e.params.TryClaim(firstRender, harken::kTargetVoiceVolume, foreign,
                e.params.TargetRevision(firstRender, harken::kTargetVoiceVolume)));
            e.variables.BeginService(e, e.mixer.BlocksRendered(), kBlockFrames);
            e.variables.PrepareClaimedSlices(e);
            publishUnrelatedPlay();
            e.params.ReleaseClaim(firstRender, harken::kTargetVoiceVolume, foreign);
            ASSERT_TRUE(e.variableSlices.AbandonPrepared(foreign));
        }
        if (variant == 2) {
            ASSERT_EQ(VN_OK, e.variables.AcceptGlobal(kVariable, 0.75f, observed,
                e.params, e.bindings, e.busTargets).result);
            EXPECT_EQ(1u, e.variables.SupersededCount());
        }
        const auto competitor = e.variables.AcceptGlobal(small, 1, observed,
            e.params, e.bindings, e.busTargets);
        ASSERT_EQ(VN_OK, competitor.result);
        harken::VariableReadCutRef cut{};
        ASSERT_TRUE(e.variables.CaptureReadCut(0, e.variables.EarliestReadCutFrame(e),
            competitor.ticket.sequence + 1, 0, &cut));
        float value = 0;
        ASSERT_TRUE(e.variables.ReadCutValue(cut, small, &value));
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            ASSERT_TRUE(e.variables.PrepareReadCutSources(e, cut));
            EXPECT_EQ(variant == 1 || variant == 2,
                e.variables.SliceForTicketForTests(competitor.ticket.sequence).Valid());
        }
        ASSERT_TRUE(e.variableSlices.AbandonPrepared(blockers[1]));
        if (variant == 0 || variant >= 3) {
            ASSERT_EQ(harken::VariableSliceTable::ReservationStatus::Ready,
                e.variableSlices.ReservationAvailability(4));
            if (variant == 3) {
                const auto play = MakePlayDesc(harkentest::ObjectId(9950));
                VnVoiceHandle laterVoice = 0;
                ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &laterVoice));
                ASSERT_LT(e.commands.SettledFrontier(), e.commands.PublishedHead());
            }
            for (unsigned attempt = 0; attempt < 16; ++attempt) {
                ASSERT_TRUE(e.variables.PrepareReadCutSources(e, cut));
                EXPECT_FALSE(e.variables.SliceForTicketForTests(competitor.ticket.sequence).Valid());
            }
        }
        if (variant == 1) {
            e.params.ReleaseClaim(firstRender, harken::kTargetVoiceVolume, foreign);
            ASSERT_TRUE(e.variableSlices.AbandonPrepared(foreign));
        }
        ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
        for (unsigned step = 0; step < 16 && e.variables.PendingCount() != 0; ++step) {
            ASSERT_EQ(VN_OK, Vn_Update());
            e.nullBackend.Pump(1);
        }
        EXPECT_EQ(0u, e.variables.PendingCount());
        EXPECT_EQ(2u, e.variables.AppliedCount());
        EXPECT_FALSE(harken::detail::VariableServiceTestAccess::HasHeldPlacements(e.variables));
        ASSERT_TRUE(e.variableSlices.AbandonPrepared(blockers[3]));
    }
}

TEST(VariableCurrentSlice, BankOnlyPrivateSourceCannotGrantTargetDebtBypass) {
  for (bool existingTargets : {false, true}) {
    SCOPED_TRACE(existingTargets);
    EngineSession session(CurrentSliceConfig(4, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto &e = *session.Engine();
    const auto small = harkentest::OffsetObjectId(kVariable, 1);
    const auto bankOnly = harkentest::OffsetObjectId(kVariable, 2);
    const std::array definitions = {
        MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0, 1,
                               0),
        MakeVariableDefinition(small, harken::VariableKindV1::Smooth, 0, 1, 0),
        MakeVariableDefinition(bankOnly, harken::VariableKindV1::Smooth, 0, 1,
                               0)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(&e, definitions.data(),
                                                  definitions.size())
                  .status);
    for (std::uint32_t i = 0; i < (existingTargets ? 4u : 3u); ++i) {
      const auto render = harkentest::RenderHandle(
          &e, PlayDc(&e, harkentest::ObjectId(9980 + i), VN_BUS_MASTER));
      ASSERT_TRUE(e.params.BindVoiceTarget(
          i < 2 ? kVariable : (i == 2 ? small : bankOnly), render,
          harken::kTargetVoiceVolume, LinearCurve(0, 0, 1, -6)));
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    ASSERT_TRUE(e.variableSlices.Reset(32, 2, existingTargets ? 3 : 2, 32));
    harken::VariableSliceRef blocker{};
    ASSERT_TRUE(e.variableSlices.Reserve(999, 1, 1, 0, 1, 1, &blocker));
    const auto observed = harken::ObserveVariableCallAt(
        e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames,
        e.timelinePool.WindowStartFrame(), e.timelinePool.PublishCutoffFrame());
    const auto ordinary = e.variables.AcceptGlobal(
        kVariable, 1, observed, e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, ordinary.result);
    const auto prepareOrdinary = [&] {
      e.variables.BeginService(e, e.mixer.BlocksRendered(), kBlockFrames);
      e.variables.PrepareClaimedSlices(e);
      ASSERT_FALSE(
          e.variables.SliceForTicketForTests(ordinary.ticket.sequence).Valid());
    };
    if (!existingTargets)
      prepareOrdinary();
    harken::PreparedVariableWriteToken bankWrite, targetWrite;
    ASSERT_EQ(
        VN_OK,
        e.variables.PrepareGlobalWrite(bankOnly, 1, observed, 0, &bankWrite)
            .result);
    ASSERT_EQ(
        VN_OK,
        e.variables.PrepareGlobalWrite(small, 1, observed, 1, &targetWrite)
            .result);
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued,
              e.logicCommits.PrepareIssue(&issue));
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(bankWrite, issue));
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(targetWrite, issue));
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(
        0, e.variables.EarliestReadCutFrame(e), issue, 2, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, bankOnly, &value));
    ASSERT_TRUE(e.variables.PrepareActionReadCutSources(e, cut, issue));
    ASSERT_TRUE(
        harken::detail::VariableServiceTestAccess::PreparedSliceForOrder(
            e.variables, issue.Sequence(), 0)
            .Valid());
    if (existingTargets)
      prepareOrdinary();
    ASSERT_TRUE(e.variableSlices.AbandonPrepared(blocker));
    ASSERT_TRUE(e.variables.ReadCutValue(cut, small, &value));
    EXPECT_EQ(existingTargets,
              e.variables.PrepareActionReadCutSources(e, cut, issue));
    EXPECT_EQ(existingTargets,
              harken::detail::VariableServiceTestAccess::PreparedSliceForOrder(
                  e.variables, issue.Sequence(), 1)
                  .Valid());
    if (existingTargets) {
      ASSERT_TRUE(e.variables.SealPreparedActionSources(issue));
      auto consumed = e.logicCommits.ConsumePreparedIssue(std::move(issue));
      e.variables.CommitPreparedActionWrite(&bankWrite, consumed);
      e.variables.CommitPreparedActionWrite(&targetWrite, consumed);
      e.variables.CommitPreparedActionSources(consumed);
      e.variables.PublishPreparedPlaybackPages();
    } else {
      e.variables.CancelPreparedWrite(&bankWrite);
      e.variables.CancelPreparedWrite(&targetWrite);
    }
    ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
    for (unsigned step = 0; step < 16 && e.variables.PendingCount() != 0;
         ++step) {
      ASSERT_EQ(VN_OK, Vn_Update());
      e.nullBackend.Pump(1);
    }
    EXPECT_EQ(0u, e.variables.PendingCount());
    EXPECT_EQ(existingTargets ? 3u : 1u, e.variables.AppliedCount());
  }
}

TEST(VariableCurrentSlice, SustainedDirectPlayDoesNotStarveOrdinaryScopedWrites) {
    constexpr std::uint32_t blockFrames = 256;
    EngineSession session(CurrentSliceConfig(32, blockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    std::array<harken::VariableDefinitionV1, 8> definitions{};
    for (std::uint32_t i = 0; i < definitions.size(); ++i)
        definitions[i] = MakeVariableDefinition(harkentest::OffsetObjectId(kVariable, i),
            harken::VariableKindV1::Smooth, 0, 1, 0);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, definitions.data(), definitions.size()).status);
    std::array<VnEmitterHandle, 128> emitters{};
    for (auto& emitter : emitters) ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const auto sound = harkentest::ObjectId(9990);
    const std::vector<float> samples(4 * blockFrames, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    RegisterVoiceAttachment(harkentest::ObjectId(9991), sound);
    for (unsigned block = 0; block < 16; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(1);
    }
    for (std::uint32_t frame = 0; frame < 96; ++frame) {
        SCOPED_TRACE(frame);
        for (std::uint32_t i = 0; i < 32; ++i) {
            const auto index = (frame * 32 + i) % 1024;
            ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitters[index / 8],
                definitions[index % 8].variableId, (frame & 1) ? 0.25f : 0.75f));
        }
        auto play = MakePlayDesc(sound);
        play.emitterHandle = emitters[(frame * 4) % emitters.size()];
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(1);
        ASSERT_LE(e.variables.PendingCount(), 256u);
        EXPECT_FALSE(harken::detail::VariableServiceTestAccess::HasHeldPlacements(e.variables));
    }
    for (unsigned block = 0; block < 16 && e.variables.PendingCount() != 0; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(1);
    }
    EXPECT_EQ(0u, e.variables.PendingCount());
    EXPECT_EQ(96u * 32u, e.variables.AppliedCount());
    EXPECT_EQ(0u, e.variables.RefusalCount());
    EXPECT_EQ(0u, e.variables.SupersededCount());
}

TEST(VariableCurrentSlice, SustainedUnrelatedPlayDoesNotStarveClaimedPhysicalTarget) {
    EngineSession session(CurrentSliceConfig(8, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto render = harkentest::RenderHandle(&e,
        PlayDc(&e, harkentest::ObjectId(9992), VN_BUS_MASTER));
    ASSERT_TRUE(e.params.BindVoiceTarget(kVariable, render, harken::kTargetVoiceVolume,
        LinearCurve(0, 0, 1, -12)));
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    const auto sound = harkentest::ObjectId(9993);
    const std::vector<float> samples(2 * kBlockFrames, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
    for (unsigned block = 0; block < 24; ++block) {
        const auto play = MakePlayDesc(sound);
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(1);
    }
    EXPECT_EQ(0u, e.variables.PendingCount());
    EXPECT_EQ(1u, e.variables.AppliedCount());
    ASSERT_NE(nullptr, e.voices.Resolve(render));
    EXPECT_NEAR(DbToLinear(-12), e.voices.Resolve(render)->gain.target, 1.0e-6f);
    EXPECT_FALSE(e.params.ForeignClaim(render, harken::kTargetVoiceVolume));
}

TEST(VariableCurrentSlice, RelocatedOrdinarySlicesPreserveSharedFilterPrefix) {
    EngineSession session(CurrentSliceConfig(8, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    const auto q = harkentest::OffsetObjectId(kVariable, 1);
    const std::array definitions = {
        MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0, 1, 0),
        MakeVariableDefinition(q, harken::VariableKindV1::Smooth, 0, 1, 0)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, definitions.data(), definitions.size()).status);
    const auto render = harkentest::RenderHandle(&e,
        PlayDc(&e, harkentest::ObjectId(9994), VN_BUS_MASTER));
    ASSERT_TRUE(e.params.BindVoiceTarget(kVariable, render, harken::kTargetVoiceLowpassCutoff,
        LinearCurve(0, 20000, 1, 1000)));
    ASSERT_TRUE(e.params.BindVoiceTarget(q, render, harken::kTargetVoiceLowpassQ,
        LinearCurve(0, 0, 1, 1)));
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    const auto sound = harkentest::ObjectId(9995);
    const std::vector<float> samples(2 * kBlockFrames, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    const auto first = AcceptGlobalWrite(kVariable, 1);
    const auto second = AcceptGlobalWrite(q, 1);
    bool checkedPrefix = false;
    for (unsigned block = 0; block < 24; ++block) {
        const auto play = MakePlayDesc(sound);
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        ASSERT_EQ(VN_OK, Vn_Update());
        const auto prior = e.variables.SliceForTicketForTests(first.sequence);
        const auto later = e.variables.SliceForTicketForTests(second.sequence);
        if (!checkedPrefix && e.variableSlices.State(later) == harken::VariableSliceState::PageCommitted) {
            ASSERT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(prior));
            ASSERT_EQ(1u, e.variableSlices.TargetCount(later));
            EXPECT_EQ(1u, e.variableSlices.SourceCount(later));
            EXPECT_LE(e.variableSlices.EffectiveFrame(prior), e.variableSlices.EffectiveFrame(later));
            const auto& slices = std::as_const(e.variableSlices);
            ASSERT_NE(nullptr, slices.Target(later, 0));
            const auto& filter = slices.Target(later, 0)->command.payload.setVoiceFilter;
            EXPECT_FLOAT_EQ(1000, filter.cutoffHz);
            EXPECT_FLOAT_EQ(harken::kToneDefaultQ * 2, filter.q);
            checkedPrefix = true;
        }
        e.nullBackend.Pump(1);
    }
    EXPECT_TRUE(checkedPrefix);
    EXPECT_EQ(0u, e.variables.PendingCount());
    EXPECT_EQ(2u, e.variables.AppliedCount());
    EXPECT_FLOAT_EQ(1000, e.params.ResolvedValue(render, harken::kTargetVoiceLowpassCutoff));
    EXPECT_FLOAT_EQ(harken::kToneDefaultQ * 2, e.params.ResolvedValue(render, harken::kTargetVoiceLowpassQ));
}

TEST(VariableCurrentSlice, OrdinaryReservationsEndWithinUpdateAcrossCallbackExits) {
    struct State { harken::Engine* engine = nullptr; VnEmitterHandle emitter = 0;
        unsigned mode = 0; bool sawHold = false; unsigned calls = 0; };
    for (unsigned mode = 0; mode < 4; ++mode) {
        SCOPED_TRACE(mode);
        State state{};
        state.mode = mode;
        auto config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
        config.notificationUserData = &state;
        config.onNotification = [](const VnNotification*, void* context) {
            auto& s = *static_cast<State*>(context);
            ++s.calls;
            s.sawHold = harken::detail::VariableServiceTestAccess::HasHeldPlacements(s.engine->variables);
            if (s.mode == 0) s.engine->nullBackend.Pump(8);
            if (s.mode == 1) (void)Vn_DestroyEmitter(s.emitter);
            if (s.mode == 2) Vn_Term();
            if (s.mode == 3) throw std::runtime_error("callback failure");
        };
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        state.engine = session.Engine();
        InstallVariable(state.engine);
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&state.emitter));
        state.engine->nullBackend.Pump(2);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_OK, Vn_SetEmitterValue(state.emitter, kVariable, 0.75f));
        VnNotification notification{};
        notification.type = VN_NOTIFY_DEGRADATION_OCCURRED;
        ASSERT_TRUE(state.engine->logicNotifications.Push(notification));
        EXPECT_EQ(mode == 3 ? VN_DEVICE_ERROR : VN_OK, Vn_Update());
        EXPECT_EQ(1u, state.calls);
        EXPECT_TRUE(state.sawHold);
        if (mode == 2) { EXPECT_EQ(nullptr, session.Engine()); continue; }
        EXPECT_FALSE(harken::detail::VariableServiceTestAccess::HasHeldPlacements(state.engine->variables));
        if (mode == 1) EXPECT_EQ(0u, state.engine->variables.PendingCount());
        if (mode == 0) {
            for (unsigned step = 0; step < 12 && state.engine->variables.PendingCount() != 0; ++step) {
                state.engine->nullBackend.Pump(1);
                ASSERT_EQ(VN_OK, Vn_Update());
            }
            EXPECT_EQ(1u, state.engine->variables.AppliedCount());
        }
    }
}

TEST(VariableCurrentSlice, LatePlacementUsesActualOrderWithinOnePageAndAcrossServiceRounds) {
    const auto cutoff = harkentest::OffsetObjectId(kVariable, 1);
    for (bool separateRounds : {false, true}) {
        SCOPED_TRACE(separateRounds);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        const std::array definitions = {
            MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0, 1, 0),
            MakeVariableDefinition(cutoff, harken::VariableKindV1::Smooth, 0, 1, 0)};
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
            harkentest::InstallDefinitionsWorld(&e, definitions.data(), 2, 0x504C4143).status);
        const auto render = harkentest::RenderHandle(&e, PlayDc(&e, harkentest::ObjectId(9980), VN_BUS_MASTER));
        ASSERT_TRUE(e.params.BindVoiceTarget(kVariable, render, harken::kTargetVoiceLowpassQ,
            LinearCurve(0, 0, 1, 1)));
        ASSERT_TRUE(e.params.BindVoiceTarget(cutoff, render, harken::kTargetVoiceLowpassCutoff,
            LinearCurve(0, 20000, 1, 1000)));
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(4);
        ASSERT_EQ(VN_OK, Vn_Update());
        auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
            e.variables.CurrentTransportEpoch(), kBlockFrames, e.timelinePool.WindowStartFrame(),
            e.timelinePool.PublishCutoffFrame());
        const auto lateFrame = e.mixer.BlocksRendered() * kBlockFrames;
        const auto accept = [&](VnVariableId variable, std::uint64_t frame) {
            observed.requestedEffectiveFrame = frame;
            return e.variables.AcceptGlobal(variable, 1, observed, e.params, e.bindings, e.busTargets);
        };
        harken::VariableAcceptResult first{}, second{};
        harken::VariableSliceRef blocker{};
        if (separateRounds) {
            ASSERT_TRUE(e.variableSlices.Reserve(e.variables.CurrentWorldGeneration(),
                e.variables.CurrentTransportEpoch(), 0xDEAD, 0, 0xDEAD, 0, &blocker));
            ASSERT_TRUE(e.params.TryClaim(render, harken::kTargetVoiceLowpassCutoff, blocker,
                e.params.TargetRevision(render, harken::kTargetVoiceLowpassCutoff)));
            first = accept(kVariable, lateFrame);
            second = accept(cutoff, lateFrame - kBlockFrames);
        } else {
            first = accept(cutoff, lateFrame);
            second = accept(kVariable, lateFrame - kBlockFrames);
        }
        ASSERT_EQ(VN_OK, first.result);
        ASSERT_EQ(VN_OK, second.result);
        e.variables.BeginService(e, e.mixer.BlocksRendered(), kBlockFrames);
        e.variables.PrepareClaimedSlices(e);
        const auto source = e.variables.SliceForTicketForTests(first.ticket.sequence);
        ASSERT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(source));
        if (separateRounds) {
            EXPECT_FALSE(e.variables.SliceForTicketForTests(second.ticket.sequence).Valid());
            e.params.ReleaseClaim(render, harken::kTargetVoiceLowpassCutoff, blocker);
            ASSERT_TRUE(e.variableSlices.AbandonPrepared(blocker));
            e.variables.BeginService(e, e.mixer.BlocksRendered(), kBlockFrames);
            e.variables.PrepareClaimedSlices(e);
        }
        const auto dependent = e.variables.SliceForTicketForTests(second.ticket.sequence);
        ASSERT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(dependent));
        EXPECT_EQ(0u, e.variableSlices.SourceCount(source));
        EXPECT_EQ(1u, e.variableSlices.SourceCount(dependent));
        const auto& slices = std::as_const(e.variableSlices);
        ASSERT_NE(nullptr, slices.Target(dependent, 0));
        const auto& filter = slices.Target(dependent, 0)->command.payload.setVoiceFilter;
        EXPECT_FLOAT_EQ(1000, filter.cutoffHz);
        if (separateRounds) {
            EXPECT_GT(e.variableSlices.EffectiveFrame(dependent), e.variableSlices.EffectiveFrame(source));
            EXPECT_FLOAT_EQ(slices.Target(source, 0)->command.payload.setVoiceFilter.q, filter.q);
        } else {
            EXPECT_EQ(e.variableSlices.EffectiveFrame(dependent), e.variableSlices.EffectiveFrame(source));
            EXPECT_GT(filter.q, slices.Target(source, 0)->command.payload.setVoiceFilter.q);
        }
        e.nullBackend.Pump(12);
        EXPECT_EQ(harken::VariableSliceState::SliceApplied, e.variableSlices.State(source));
        EXPECT_EQ(harken::VariableSliceState::SliceApplied, e.variableSlices.State(dependent));
        e.variables.EndService(e);
        EXPECT_EQ(2u, e.variables.AppliedCount());
    }
}

TEST(VariableCurrentSlice, ReadCutsCaptureAllVariablesAndPinOnlyReadBatchSources) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    std::array<harken::VariableDefinitionV1, harken::kVariableDefinitionCapacity> definitions{};
    for (std::uint32_t index = 0; index < definitions.size(); ++index) {
        definitions[index] = MakeVariableDefinition(harkentest::OffsetObjectId(kVariable, index),
            harken::VariableKindV1::Smooth, 0, 300, static_cast<float>(index));
    }
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, definitions.data(), definitions.size(), 0x43555453).status);
    const auto before = harken::detail::VariableServiceTestAccess::OutcomeCapacity(e.variables)[2];
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    const VnVariableWriteV1 writes[] = {
        {definitions[200].variableId, 12}, {definitions[201].variableId, 13}, {definitions[255].variableId, 14}};
    const auto accepted = e.variables.AcceptGlobalBatch(writes, 3, observed,
        e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, accepted.result);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, accepted.ticket.sequence + 1, 0, &cut));
    float value = -1;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, definitions[255].variableId, &value));
    EXPECT_FLOAT_EQ(14, value);
    ASSERT_TRUE(e.variables.ReadCutValue(cut, definitions[200].variableId, &value));
    EXPECT_FLOAT_EQ(12, value);
    ASSERT_TRUE(e.variables.ReadCutValue(cut, definitions[199].variableId, &value));
    EXPECT_FLOAT_EQ(199, value);
    const auto unrelated = e.variables.AcceptGlobal(definitions[199].variableId, 5, observed,
        e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, unrelated.result);
    const VnVariableWriteV1 newer[] = {{definitions[200].variableId, 30}, {definitions[201].variableId, 31}};
    ASSERT_EQ(VN_OK, e.variables.AcceptGlobalBatch(newer, 2, observed,
        e.params, e.bindings, e.busTargets).result);
    EXPECT_EQ(0u, e.variables.SupersededCount());
    e.variables.BeginService(e, 0, kBlockFrames);
    e.variables.PrepareClaimedSlices(e);
    EXPECT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    e.nullBackend.Pump(12);
    EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(cut));
    e.variables.EndService(e);
    e.variables.MarkOutcomeDeliveryComplete();
    e.variables.FinishOutcomeDelivery();
    EXPECT_EQ(before - 1, harken::detail::VariableServiceTestAccess::OutcomeCapacity(e.variables)[2]);
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
    EXPECT_EQ(before, harken::detail::VariableServiceTestAccess::OutcomeCapacity(e.variables)[2]);
    EXPECT_FALSE(e.variables.ReleaseReadCut(cut));
    EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.ReadCutSourcesStatus(cut));
}

TEST(VariableCurrentSlice, ReadCutUsesFrameSequenceOrdinalAndFailsOnlyForObservedCanceledSource) {
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::PreparedVariableWriteToken a, b, future;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.25f, observed, 1, &a).result);
    ASSERT_EQ(VN_OK, e.variables.PrepareEmitterWrite(emitter, kVariable, 0.75f, observed, 3,
        e.emitters, &b).result);
    observed.requestedEffectiveFrame = 6 * kBlockFrames + 1;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 1, observed, 0, &future).result);
    harken::LogicCommitSequence sequence = 0;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.TryIssue(&sequence));
    e.variables.CommitPreparedWrite(&a, sequence);
    e.variables.CommitPreparedWrite(&b, sequence);
    e.variables.CommitPreparedWrite(&future, sequence);
    harken::VariableReadCutRef first{}, second{}, independent{};
    ASSERT_TRUE(e.variables.CaptureReadCut(emitter, 4 * kBlockFrames, sequence, 2, &first));
    ASSERT_TRUE(e.variables.CaptureReadCut(emitter, 4 * kBlockFrames, sequence, 4, &second));
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, sequence, 2, &independent));
    float value = -1;
    ASSERT_TRUE(e.variables.ReadCutValue(first, kVariable, &value));
    EXPECT_FLOAT_EQ(0.25f, value);
    ASSERT_TRUE(e.variables.ReadCutValue(second, kVariable, &value));
    EXPECT_FLOAT_EQ(0.75f, value);
    e.variables.BeginService(e, 0, kBlockFrames);
    e.variables.PrepareClaimedSlices(e);
    EXPECT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(first, 4 * kBlockFrames));
    EXPECT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(second, 4 * kBlockFrames));
    EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(independent, 4 * kBlockFrames));
    e.variables.NoteEmitterDestroyed(e, emitter);
    e.nullBackend.Pump(12);
    EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(first));
    EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.ReadCutSourcesStatus(second));
    EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(independent));
    EXPECT_TRUE(e.variables.ReleaseReadCut(first));
    EXPECT_TRUE(e.variables.ReleaseReadCut(second));
    EXPECT_TRUE(e.variables.ReleaseReadCut(independent));
}

TEST(VariableCurrentSlice, ReadCutMustRebuildWhenFinalPlacementChangesTheWinningWrite) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    observed.requestedEffectiveFrame = 2 * kBlockFrames;
    const auto first = e.variables.AcceptGlobal(kVariable, 0.25f, observed,
        e.params, e.bindings, e.busTargets);
    observed.requestedEffectiveFrame = kBlockFrames;
    const auto second = e.variables.AcceptGlobal(kVariable, 0.75f, observed,
        e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, first.result);
    ASSERT_EQ(VN_OK, second.result);
    harken::VariableReadCutRef original{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 8 * kBlockFrames, second.ticket.sequence + 1, 0, &original));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(original, kVariable, &value));
    EXPECT_FLOAT_EQ(0.25f, value);
    e.variables.BeginService(e, 3, kBlockFrames);
    e.variables.PrepareClaimedSlices(e);
    const auto a = e.variables.SliceForTicketForTests(first.ticket.sequence);
    const auto b = e.variables.SliceForTicketForTests(second.ticket.sequence);
    ASSERT_EQ(e.variableSlices.EffectiveFrame(a), e.variableSlices.EffectiveFrame(b));
    EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.PublishReadCut(original, 8 * kBlockFrames));
    ASSERT_TRUE(e.variables.ReleaseReadCut(original));
    harken::VariableReadCutRef rebuilt{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 8 * kBlockFrames, second.ticket.sequence + 1, 0, &rebuilt));
    ASSERT_TRUE(e.variables.ReadCutValue(rebuilt, kVariable, &value));
    EXPECT_FLOAT_EQ(0.75f, value);
    EXPECT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(rebuilt, 8 * kBlockFrames));
    e.nullBackend.Pump(12);
    EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(rebuilt));
    ASSERT_TRUE(e.variables.ReleaseReadCut(rebuilt));
}

TEST(VariableCurrentSlice, ReadCutAndOutcomeReleaseOrdersBothRespectTheAudioFence) {
    using Access = harken::detail::VariableServiceTestAccess;
    for (bool releaseCutFirst : {false, true}) {
        SCOPED_TRACE(releaseCutFirst);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const auto freeBefore = Access::OutcomeCapacity(e.variables)[2];
        auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
            e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
        const auto accepted = e.variables.AcceptGlobal(kVariable, 0.5f, observed,
            e.params, e.bindings, e.busTargets);
        ASSERT_EQ(VN_OK, accepted.result);
        harken::VariableReadCutRef cut{};
        ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, accepted.ticket.sequence + 1, 0, &cut));
        float value = 0;
        ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
        e.variables.BeginService(e, 0, kBlockFrames);
        e.variables.PrepareClaimedSlices(e);
        const auto source = e.variables.SliceForTicketForTests(accepted.ticket.sequence);
        EXPECT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
        for (std::uint32_t step = 0; step < 12 &&
            e.variables.ReadCutSourcesStatus(cut) == harken::VariableReadCutStatus::Waiting; ++step) e.nullBackend.Pump(1);
        ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(cut));
        ASSERT_EQ(e.mixer.BlocksRendered(), e.variableSlices.AppliedBlockSequence(source));
        e.variables.EndService(e);
        if (releaseCutFirst) ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
        e.variables.MarkOutcomeDeliveryComplete();
        e.variables.FinishOutcomeDelivery();
        if (!releaseCutFirst) ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
        EXPECT_EQ(freeBefore - 1, Access::OutcomeCapacity(e.variables)[2]);
        EXPECT_EQ(harken::VariableSliceState::SliceApplied, e.variableSlices.State(source));
        e.nullBackend.Pump(1);
        e.variables.EndService(e);
        EXPECT_EQ(freeBefore, Access::OutcomeCapacity(e.variables)[2]);
        EXPECT_EQ(harken::VariableSliceState::Free, e.variableSlices.State(source));
    }
}

TEST(VariableCurrentSlice, PublishedReadCutKeepsProofWhileAppliedPayloadAndTargetsAreReused) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    ASSERT_TRUE(e.variableSlices.Reset(4, 1, 1, 1));
    InstallVariable(&e);
    RegisterBusVolumeBinding(harkentest::ObjectId(1), VN_BUS_MASTER);
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    const auto first = e.variables.AcceptGlobal(kVariable, 0.25f, observed,
        e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, first.result);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames,
        first.ticket.sequence + 1, 0, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    e.variables.BeginService(e, 0, kBlockFrames);
    e.variables.PrepareClaimedSlices(e);
    const auto proof = e.variables.SliceForTicketForTests(first.ticket.sequence);
    ASSERT_TRUE(proof.Valid());
    ASSERT_EQ(1u, e.variableSlices.TargetCount(proof));
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting,
        e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    for (int step = 0; step < 12 &&
         e.variables.ReadCutSourcesStatus(cut) == harken::VariableReadCutStatus::Waiting; ++step)
        e.nullBackend.Pump(1);
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(cut));
    e.variables.EndService(e);
    e.variables.MarkOutcomeDeliveryComplete();
    e.variables.FinishOutcomeDelivery();
    EXPECT_EQ(0u, e.variableSlices.FreePayloadCount());
    e.nullBackend.Pump(1);
    e.variables.EndService(e);
    ASSERT_EQ(1u, e.variableSlices.FreePayloadCount());
    ASSERT_EQ(1u, e.variableSlices.FreeTargetCapacity());
    EXPECT_EQ(0u, e.variableSlices.TargetCount(proof));
    EXPECT_EQ(harken::VariableSliceState::SliceApplied, e.variableSlices.State(proof));

    observed.requestedEffectiveFrame = (e.mixer.BlocksRendered() + 4) * kBlockFrames;
    const auto next = e.variables.AcceptGlobal(kVariable, 1.0f, observed,
        e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, next.result);
    for (int step = 0; step < 12; ++step) {
        ASSERT_EQ(VN_OK, Vn_Update());
        e.nullBackend.Pump(1);
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(-12.0f, e.busTargets.LastCommitted(0, harken::kBusTargetBusVolume));
    EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(cut));
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    EXPECT_FLOAT_EQ(0.25f, value);
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(4u, e.variableSlices.FreeRowCount());
    EXPECT_EQ(1u, e.variableSlices.FreePayloadCount());
    EXPECT_EQ(1u, e.variableSlices.FreeTargetCapacity());
}

TEST(VariableCurrentSlice, ReadCutCapacityRefusalAndCanceledPreparationReleaseEveryObserver) {
    using Access = harken::detail::VariableServiceTestAccess;
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto freeBefore = Access::OutcomeCapacity(e.variables)[2];
    const auto storage = e.variables.ReadCutStorageBytes();
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::PreparedVariableWriteToken prepared;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.5f, observed, 0, &prepared).result);
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(prepared, issue));
    std::vector<harken::VariableReadCutRef> cuts(e.variables.ReadCutCapacity());
    for (auto& cut : cuts) {
        ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 4 * kBlockFrames, issue, 1, &cut));
        float value = 0;
        ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
        EXPECT_FLOAT_EQ(0.5f, value);
    }
    harken::VariableReadCutRef refused{};
    EXPECT_FALSE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 1, 1, &refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(storage, e.variables.ReadCutStorageBytes());
    e.variables.CancelPreparedWrite(&prepared);
    EXPECT_EQ(freeBefore - 1, Access::OutcomeCapacity(e.variables)[2]);
    for (const auto cut : cuts) {
        EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
        EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
    }
    EXPECT_EQ(freeBefore, Access::OutcomeCapacity(e.variables)[2]);
    harken::VariableReadCutRef reused{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 1, 1, &reused));
    EXPECT_NE(cuts.front().generation, reused.generation);
    EXPECT_FALSE(e.variables.ReleaseReadCut(cuts.front()));
    EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(reused, 4 * kBlockFrames));
    EXPECT_TRUE(e.variables.ReleaseReadCut(reused));
}

TEST(VariableCurrentSlice, ActionReadCutRequiresTheOriginalReservedWriteAndIssuer) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::PreparedVariableWriteToken original, foreign, replacement;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.25f, observed, 0, &original).result);
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.9f, observed, 1, &foreign).result);
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(original, issue));
    harken::VariableReadCutRef plain{}, first{}, next{};
    float value = -1;
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, issue.Sequence(), 2, &plain));
    ASSERT_TRUE(e.variables.ReadCutValue(plain, kVariable, &value));
    EXPECT_FLOAT_EQ(0, value);
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 4 * kBlockFrames, issue, 2, &first));
    ASSERT_TRUE(e.variables.ReadCutValue(first, kVariable, &value));
    EXPECT_FLOAT_EQ(0.25f, value);
    const auto sequence = issue.Sequence();
    issue.Release();
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    ASSERT_EQ(sequence, issue.Sequence());
    EXPECT_FALSE(e.variables.BindPreparedActionWriteOrder(original, issue));
    EXPECT_FALSE(e.variables.PrepareActionReadCutSources(e, first, issue));
    EXPECT_FALSE(e.variables.ReadCutValue(first, kVariable, &value));
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.75f, observed, 0, &replacement).result);
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(replacement, issue));
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 4 * kBlockFrames, issue, 2, &next));
    ASSERT_TRUE(e.variables.ReadCutValue(next, kVariable, &value));
    EXPECT_FLOAT_EQ(0.75f, value);
    e.variables.CancelPreparedWrite(&original);
    EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.ReadCutSourcesStatus(first));
    EXPECT_TRUE(e.variables.ReadCutValue(next, kVariable, &value));
    e.variables.CancelPreparedWrite(&replacement);
    e.variables.CancelPreparedWrite(&foreign);
    EXPECT_TRUE(e.variables.ReleaseReadCut(plain));
    EXPECT_TRUE(e.variables.ReleaseReadCut(first));
    EXPECT_TRUE(e.variables.ReleaseReadCut(next));
}

TEST(VariableCurrentSlice, ActionPreparedSourceCannotApplyBeforeWholeActionCommit) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::PreparedVariableWriteToken write;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.5f, observed, 0, &write).result);
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(write, issue));
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 4 * kBlockFrames, issue, 1, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    ASSERT_FLOAT_EQ(0.5f, value);
    ASSERT_TRUE(e.variables.PrepareActionReadCutSources(e, cut, issue));
    const auto slice = harken::detail::VariableServiceTestAccess::PreparedSliceForOrder(e.variables, issue.Sequence(), 0);
    ASSERT_TRUE(slice.Valid());
    EXPECT_EQ(harken::VariableSliceState::Prepared, e.variableSlices.State(slice));
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    ASSERT_TRUE(e.variables.SealPreparedActionSources(issue));
    e.timelinePool.Publish(5 * kBlockFrames);
    e.nullBackend.Pump(2);
    EXPECT_EQ(0u, e.variables.AcceptedCount());
    EXPECT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.ReadCutSourcesStatus(cut));
    auto consumed = e.logicCommits.ConsumePreparedIssue(std::move(issue));
    e.variables.CommitPreparedActionWrite(&write, consumed);
    e.variables.CommitPreparedActionSources(consumed);
    e.variables.PublishPreparedPlaybackPages();
    EXPECT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(slice));
    for (std::uint32_t step = 0; step < 12 &&
        e.variables.ReadCutSourcesStatus(cut) == harken::VariableReadCutStatus::Waiting; ++step) e.nullBackend.Pump(1);
    EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(cut));
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, DeferredChildReadCutPublishesOnlyAfterActualActivationWithInheritedOrder) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::PreparedActionFiring root, child;
    ASSERT_TRUE(e.actionFirings.PrepareRoot(harkentest::ObjectId(31), e.variables.CurrentWorldGeneration(), 1, &root));
    const auto inherited = e.logicCommits.Issue();
    ASSERT_TRUE(e.actionFirings.Commit(&root, inherited));
    ASSERT_TRUE(e.actionFirings.PrepareChild(root.Handle(), harkentest::ObjectId(32),
        harken::ActionFiringOwnerStamp::DirectWorld(e.variables.CurrentWorldGeneration()), 1, &child));
    harken::ActionFiringTable::PreparedChildIdentity issuer;
    ASSERT_TRUE(e.actionFirings.CapturePreparedChildIdentity(child, e.actionFirings.CancellationEpoch(root.Handle()), &issuer));
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::PreparedVariableWriteToken write;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.5f, observed, 0, &write).result);
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(write, e, issuer));
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(e, 0, 4 * kBlockFrames, issuer, 1, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    ASSERT_FLOAT_EQ(0.5f, value);
    ASSERT_TRUE(e.variables.PrepareActionReadCutSources(e, cut, issuer));
    const auto slice = harken::detail::VariableServiceTestAccess::PreparedSliceForOrder(e.variables, inherited, 0);
    ASSERT_TRUE(slice.Valid());
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    ASSERT_TRUE(e.variables.SealPreparedActionSources(e, issuer));
    e.variables.CommitPreparedWrite(&write, inherited);
    EXPECT_TRUE(write.Valid());
    harken::LogicCommitCoordinator sameNumber;
    harken::LogicCommitCoordinator::PreparedIssue ordinary;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, sameNumber.PrepareIssue(&ordinary));
    ASSERT_EQ(inherited, ordinary.Sequence());
    auto wrongReceipt = sameNumber.ConsumePreparedIssue(std::move(ordinary));
    e.variables.CommitPreparedActionWrite(&write, wrongReceipt);
    e.variables.CommitPreparedActionSources(wrongReceipt);
    EXPECT_TRUE(write.Valid());
    EXPECT_EQ(harken::VariableSliceState::Prepared, e.variableSlices.State(slice));
    e.timelinePool.Publish(5 * kBlockFrames);
    e.nullBackend.Pump(2);
    EXPECT_EQ(0u, e.variables.AcceptedCount());
    harken::ActionFiringTable::ChildActivationReceipt receipt;
    ASSERT_TRUE(e.actionFirings.CommitPreparedChild(&child, &receipt));
    e.variables.CommitPreparedActionWrite(&write, receipt);
    ASSERT_FALSE(write.Valid());
    e.variables.CommitPreparedActionSources(receipt);
    e.variables.PublishPreparedPlaybackPages();
    EXPECT_EQ(inherited, e.logicCommits.Current());
    EXPECT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(slice));
    for (std::uint32_t step = 0; step < 12 &&
        e.variables.ReadCutSourcesStatus(cut) == harken::VariableReadCutStatus::Waiting; ++step) e.nullBackend.Pump(1);
    EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(cut));
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, PendingCutIncludesLaterMaterializedEarlierChildAndFiltersForeignOrder) {
    using Access = harken::detail::VariableServiceTestAccess;
    for (unsigned variant = 0; variant < 6; ++variant) {
        SCOPED_TRACE(variant);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        VnEmitterHandle emitter = 0, foreign = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&foreign));
        const auto owner = harken::ActionFiringOwnerStamp::DirectWorld(e.variables.CurrentWorldGeneration());
        harken::PreparedActionFiring root, child;
        ASSERT_TRUE(e.actionFirings.PrepareRoot(harkentest::ObjectId(31), owner, 1, &root));
        harken::LogicCommitSequence rootSequence = 0;
        if (variant == 5) {
            harken::LogicCommitCoordinator::PreparedIssue rootIssue;
            ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&rootIssue));
            auto rootConsumed = e.logicCommits.ConsumePreparedIssue(std::move(rootIssue));
            rootSequence = rootConsumed.Sequence();
            ASSERT_TRUE(e.actionFirings.Commit(&root, rootSequence));
        }
        harken::LogicCommitCoordinator::PreparedIssue issue;
        ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
        harken::VariableReadCutRef pending{}, published{};
        ASSERT_TRUE(e.variables.CaptureActionReadCut(emitter, 4 * kBlockFrames, issue, 2, &pending, true));
        ASSERT_TRUE(e.variables.CaptureActionReadCut(emitter, 4 * kBlockFrames, issue, 2, &published, true));
        float value = -1;
        ASSERT_TRUE(e.variables.ReadCutValue(pending, kVariable, &value));
        ASSERT_FLOAT_EQ(0.0f, value);
        auto consumed = e.logicCommits.ConsumePreparedIssue(std::move(issue));
        if (variant != 5) {
            rootSequence = consumed.Sequence();
            ASSERT_TRUE(e.actionFirings.Commit(&root, rootSequence));
        } else {
            ASSERT_LT(rootSequence, consumed.Sequence());
        }
        e.variables.CommitPreparedActionSources(consumed);
        ASSERT_TRUE(Access::RebuildPending(e.variables, pending, 8 * kBlockFrames));
        ASSERT_TRUE(e.variables.ReadCutValue(pending, kVariable, &value));
        ASSERT_TRUE(Access::RebuildPending(e.variables, published, 8 * kBlockFrames));
        ASSERT_TRUE(e.variables.ReadCutValue(published, kVariable, &value));
        ASSERT_EQ(harken::VariableReadCutStatus::Applied,
            e.variables.PublishReadCut(published, 8 * kBlockFrames));
        auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
            e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
        observed.requestedEffectiveFrame = 6 * kBlockFrames;
        const std::uint32_t ordinal = variant == 1 ? 2 : 0;
        if (variant == 2) {
            ASSERT_EQ(VN_OK, e.variables.AcceptGlobal(kVariable, 0.75f, observed,
                e.params, e.bindings, e.busTargets).result);
        } else {
            ASSERT_TRUE(e.actionFirings.PrepareChild(root.Handle(), harkentest::ObjectId(32), owner, 1, &child));
            harken::ActionFiringTable::PreparedChildIdentity issuer;
            ASSERT_TRUE(e.actionFirings.CapturePreparedChildIdentity(child, 1, &issuer));
            harken::PreparedVariableWriteToken write;
            if (variant < 2) {
                ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.75f, observed, ordinal, &write).result);
            } else {
                ASSERT_EQ(VN_OK, e.variables.PrepareEmitterWrite(variant == 3 ? foreign : emitter,
                    kVariable, 0.75f, observed, ordinal, e.emitters, &write).result);
            }
            ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(write, e, issuer));
            harken::ActionFiringTable::ChildActivationReceipt receipt;
            ASSERT_TRUE(e.actionFirings.CommitPreparedChild(&child, &receipt));
            e.variables.CommitPreparedActionWrite(&write, receipt);
            ASSERT_FALSE(write.Valid());
            e.variables.CommitPreparedActionSources(receipt);
        }
        const bool included = variant == 0 || variant == 4 || variant == 5;
        EXPECT_EQ(included ? 1u : 0u, Access::CandidateCount(e.variables, pending));
        EXPECT_EQ(0u, Access::CandidateCount(e.variables, published));
        ASSERT_TRUE(e.variables.ReadCutValue(published, kVariable, &value));
        EXPECT_FLOAT_EQ(0.0f, value);
        if (included) EXPECT_FALSE(e.variables.ReadCutValue(pending, kVariable, &value));
        ASSERT_TRUE(Access::RebuildPending(e.variables, pending, 8 * kBlockFrames));
        ASSERT_TRUE(e.variables.ReadCutValue(pending, kVariable, &value));
        ASSERT_FLOAT_EQ(included ? 0.75f : 0.0f, value);
        if (included) {
            EXPECT_EQ(2u, Access::ObserversForOrder(e.variables, rootSequence, ordinal));
            e.variables.BeginService(e, e.mixer.BlocksRendered(), kBlockFrames);
            e.variables.PrepareClaimedSlices(e);
            e.variables.PublishPreparedPlaybackPages(9 * kBlockFrames);
            ASSERT_EQ(harken::VariableReadCutStatus::Waiting,
                e.variables.PublishReadCut(pending, 8 * kBlockFrames));
            e.nullBackend.Pump(9);
            EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(pending));
        }
        ASSERT_TRUE(e.variables.ReleaseReadCut(pending));
        ASSERT_TRUE(e.variables.ReleaseReadCut(published));
        EXPECT_EQ(0u, Access::ObserversForOrder(e.variables, rootSequence, ordinal));
    }
}

TEST(VariableCurrentSlice, PendingCutPinsAndReleasesCandidatesBeyond4096IncludingLateInheritedWrite) {
    using Access = harken::detail::VariableServiceTestAccess;
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    const auto other = harkentest::OffsetObjectId(kVariable, 1);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallSmoothDefinitions(&e, {kVariable, other}, 0, 1).status);
    const auto freeBefore = Access::OutcomeCapacity(e.variables)[2];
    harken::PreparedActionFiring root, child;
    const auto owner = harken::ActionFiringOwnerStamp::DirectWorld(e.variables.CurrentWorldGeneration());
    ASSERT_TRUE(e.actionFirings.PrepareRoot(harkentest::ObjectId(31), owner, 1, &root));
    const auto inherited = e.logicCommits.Issue();
    ASSERT_TRUE(e.actionFirings.Commit(&root, inherited));
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    constexpr std::uint32_t count = 4097;
    ASSERT_GT(e.variables.PendingCapacity(), count + 1);
    for (std::uint32_t i = 0; i < count; ++i) {
        observed.requestedEffectiveFrame = static_cast<std::uint64_t>(32u + i) * kBlockFrames;
        ASSERT_EQ(VN_OK, e.variables.AcceptGlobal(other, 0.25f, observed,
            e.params, e.bindings, e.busTargets).result);
    }
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 4 * kBlockFrames, issue, 2, &cut, true));
    ASSERT_EQ(count, Access::CandidateCount(e.variables, cut));
    float value = -1;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    ASSERT_FLOAT_EQ(0.0f, value);
    auto consumed = e.logicCommits.ConsumePreparedIssue(std::move(issue));
    e.variables.CommitPreparedActionSources(consumed);
    ASSERT_TRUE(Access::RebuildPending(e.variables, cut, 8 * kBlockFrames));
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    ASSERT_TRUE(e.actionFirings.PrepareChild(root.Handle(), harkentest::ObjectId(32), owner, 1, &child));
    harken::ActionFiringTable::PreparedChildIdentity issuer;
    ASSERT_TRUE(e.actionFirings.CapturePreparedChildIdentity(child, 1, &issuer));
    observed.requestedEffectiveFrame = 6 * kBlockFrames;
    harken::PreparedVariableWriteToken write;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.75f, observed, 0, &write).result);
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(write, e, issuer));
    harken::ActionFiringTable::ChildActivationReceipt receipt;
    ASSERT_TRUE(e.actionFirings.CommitPreparedChild(&child, &receipt));
    e.variables.CommitPreparedActionWrite(&write, receipt);
    ASSERT_FALSE(write.Valid());
    e.variables.CommitPreparedActionSources(receipt);
    ASSERT_GE(Access::HistorySlotForOrder(e.variables, inherited, 0), 4096u);
    EXPECT_EQ(count + 1u, Access::CandidateCount(e.variables, cut));
    EXPECT_FALSE(e.variables.ReadCutValue(cut, kVariable, &value));
    ASSERT_TRUE(Access::RebuildPending(e.variables, cut, 8 * kBlockFrames));
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    EXPECT_FLOAT_EQ(0.75f, value);
    EXPECT_EQ(2u, Access::ObserversForOrder(e.variables, inherited, 0));
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
    EXPECT_EQ(0u, Access::TotalObservers(e.variables));
    e.variables.TerminateSession();
    e.CompleteVariableOperation();
    EXPECT_EQ(freeBefore, Access::OutcomeCapacity(e.variables)[2]);
}

TEST(VariableCurrentSlice, PendingCutFailedCandidateAffectsOnlyActualReadScopeAndFrame) {
    using Access = harken::detail::VariableServiceTestAccess;
    for (unsigned variant = 0; variant < 3; ++variant) {
        SCOPED_TRACE(variant);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        const auto other = harkentest::ObjectId(7002);
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
            harkentest::InstallSmoothDefinitions(&e, {kVariable, other}, 0, 1).status);
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        harken::LogicCommitCoordinator::PreparedIssue issue;
        ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
        auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
            e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
        observed.requestedEffectiveFrame = (variant == 2 ? 12u : 4u) * kBlockFrames;
        harken::PreparedVariableWriteToken write;
        if (variant == 1) {
            ASSERT_EQ(VN_OK, e.variables.PrepareEmitterWrite(emitter, kVariable, 0.75f,
                observed, 0, e.emitters, &write).result);
        } else {
            ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.75f, observed, 0, &write).result);
        }
        ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(write, issue));
        harken::VariableReadCutRef cut{};
        ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 4 * kBlockFrames, issue, 1, &cut, true));
        auto consumed = e.logicCommits.ConsumePreparedIssue(std::move(issue));
        e.variables.CommitPreparedActionSources(consumed);
        e.variables.CancelPreparedWrite(&write);
        ASSERT_FALSE(write.Valid());
        ASSERT_TRUE(Access::RebuildPending(e.variables, cut, 8 * kBlockFrames));
        float value = -1;
        ASSERT_TRUE(e.variables.ReadCutValue(cut, other, &value));
        EXPECT_FLOAT_EQ(0.0f, value);
        if (variant == 0) {
            EXPECT_FALSE(e.variables.ReadCutValue(cut, kVariable, &value));
        } else {
            ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
            EXPECT_FLOAT_EQ(0.0f, value);
        }
        EXPECT_EQ(variant == 1 ? 0u : 1u, Access::CandidateCount(e.variables, cut));
        EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, 8 * kBlockFrames));
        ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
    }
}

TEST(VariableCurrentSlice, PendingCutBatchNonleaderReadsRetainActualLeaderThroughAppliedAndRelease) {
    using Access = harken::detail::VariableServiceTestAccess;
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    const auto other = harkentest::ObjectId(7002);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallSmoothDefinitions(&e, {kVariable, other}, 0, 1).status);
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    observed.requestedEffectiveFrame = 6 * kBlockFrames;
    const VnVariableWriteV1 writes[] = {{kVariable, 0.25f}, {other, 0.75f}};
    const auto accepted = e.variables.AcceptGlobalBatch(writes, 2, observed, e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, accepted.result);
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 4 * kBlockFrames, issue, 1, &cut, true));
    auto consumed = e.logicCommits.ConsumePreparedIssue(std::move(issue));
    e.variables.CommitPreparedActionSources(consumed);
    ASSERT_TRUE(Access::RebuildPending(e.variables, cut, 8 * kBlockFrames));
    float value = -1;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, other, &value));
    EXPECT_FLOAT_EQ(0.75f, value);
    EXPECT_EQ(2u, Access::CandidateCount(e.variables, cut));
    EXPECT_EQ(2u, Access::ObserversForVariable(e.variables, accepted.ticket.sequence, kVariable));
    EXPECT_EQ(1u, Access::ObserversForVariable(e.variables, accepted.ticket.sequence, other));
    e.variables.BeginService(e, e.mixer.BlocksRendered(), kBlockFrames);
    e.variables.PrepareClaimedSlices(e);
    e.variables.PublishPreparedPlaybackPages(9 * kBlockFrames);
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 8 * kBlockFrames));
    e.nullBackend.Pump(9);
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(cut));
    ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
    EXPECT_EQ(0u, Access::ObserversForVariable(e.variables, accepted.ticket.sequence, kVariable));
    EXPECT_EQ(0u, Access::ObserversForVariable(e.variables, accepted.ticket.sequence, other));
}

TEST(VariableCurrentSlice, PendingCutLateObserverOverflowRemainsFailedAcrossRebuild) {
    using Access = harken::detail::VariableServiceTestAccess;
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::PreparedActionFiring root, child;
    const auto owner = harken::ActionFiringOwnerStamp::DirectWorld(e.variables.CurrentWorldGeneration());
    ASSERT_TRUE(e.actionFirings.PrepareRoot(harkentest::ObjectId(31), owner, 1, &root));
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 4 * kBlockFrames, issue, 1, &cut, true));
    auto consumed = e.logicCommits.ConsumePreparedIssue(std::move(issue));
    ASSERT_TRUE(e.actionFirings.Commit(&root, consumed.Sequence()));
    e.variables.CommitPreparedActionSources(consumed);
    ASSERT_TRUE(e.actionFirings.PrepareChild(root.Handle(), harkentest::ObjectId(32), owner, 1, &child));
    harken::ActionFiringTable::PreparedChildIdentity issuer;
    ASSERT_TRUE(e.actionFirings.CapturePreparedChildIdentity(child, 1, &issuer));
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::PreparedVariableWriteToken write;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.75f, observed, 0, &write).result);
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(write, e, issuer));
    ASSERT_TRUE(Access::SeedObserversForOrder(e.variables, consumed.Sequence(), 0, UINT32_MAX));
    harken::ActionFiringTable::ChildActivationReceipt receipt;
    ASSERT_TRUE(e.actionFirings.CommitPreparedChild(&child, &receipt));
    e.variables.CommitPreparedActionWrite(&write, receipt);
    ASSERT_FALSE(write.Valid());
    e.variables.CommitPreparedActionSources(receipt);
    ASSERT_TRUE(Access::SeedObserversForOrder(e.variables, consumed.Sequence(), 0, 0));
    EXPECT_EQ(0u, Access::CandidateCount(e.variables, cut));
    EXPECT_FALSE(Access::RebuildPending(e.variables, cut, 4 * kBlockFrames));
    EXPECT_FALSE(Access::RebuildPending(e.variables, cut, 8 * kBlockFrames));
    float value = -1;
    EXPECT_FALSE(e.variables.ReadCutValue(cut, kVariable, &value));
    EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, DeferredSiblingWithSameOrderCannotCommitAnotherChildsSources) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::PreparedActionFiring root, child, sibling;
    ASSERT_TRUE(e.actionFirings.PrepareRoot(harkentest::ObjectId(31), e.variables.CurrentWorldGeneration(), 1, &root));
    const auto inherited = e.logicCommits.Issue();
    ASSERT_TRUE(e.actionFirings.Commit(&root, inherited));
    const auto owner = harken::ActionFiringOwnerStamp::DirectWorld(e.variables.CurrentWorldGeneration());
    ASSERT_TRUE(e.actionFirings.PrepareChild(root.Handle(), harkentest::ObjectId(32), owner, 1, &child));
    ASSERT_TRUE(e.actionFirings.PrepareChild(root.Handle(), harkentest::ObjectId(33), owner, 1, &sibling));
    harken::ActionFiringTable::PreparedChildIdentity issuer;
    ASSERT_TRUE(e.actionFirings.CapturePreparedChildIdentity(child, 1, &issuer));
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::PreparedVariableWriteToken write;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.5f, observed, 0, &write).result);
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(write, e, issuer));
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(e, 0, 4 * kBlockFrames, issuer, 1, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    ASSERT_TRUE(e.variables.PrepareActionReadCutSources(e, cut, issuer));
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    ASSERT_TRUE(e.variables.SealPreparedActionSources(e, issuer));
    harken::ActionFiringTable::ChildActivationReceipt wrongReceipt, receipt;
    ASSERT_TRUE(e.actionFirings.CommitPreparedChild(&sibling, &wrongReceipt));
    e.variables.CommitPreparedActionWrite(&write, wrongReceipt);
    e.variables.CommitPreparedActionSources(wrongReceipt);
    EXPECT_TRUE(write.Valid());
    EXPECT_EQ(0u, e.variables.AcceptedCount());
    ASSERT_TRUE(e.actionFirings.CommitPreparedChild(&child, &receipt));
    auto movedReceipt = std::move(receipt);
    e.variables.CommitPreparedActionWrite(&write, receipt);
    EXPECT_TRUE(write.Valid());
    e.variables.CommitPreparedActionWrite(&write, movedReceipt);
    ASSERT_FALSE(write.Valid());
    e.variables.CommitPreparedActionSources(movedReceipt);
    EXPECT_EQ(1u, e.variables.AcceptedCount());
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, DeferredPrivateSourcesRejectForeignTableAndCancelledOriginalChild) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::ActionFiringTable foreign;
    ASSERT_TRUE(foreign.Reset(2));
    harken::PreparedActionFiring root, child, foreignRoot, foreignChild;
    const auto owner = harken::ActionFiringOwnerStamp::DirectWorld(e.variables.CurrentWorldGeneration());
    ASSERT_TRUE(e.actionFirings.PrepareRoot(harkentest::ObjectId(31), owner, 1, &root));
    const auto inherited = e.logicCommits.Issue();
    ASSERT_TRUE(e.actionFirings.Commit(&root, inherited));
    ASSERT_TRUE(e.actionFirings.PrepareChild(root.Handle(), harkentest::ObjectId(32), owner, 1, &child));
    ASSERT_TRUE(foreign.PrepareRoot(harkentest::ObjectId(31), owner, 1, &foreignRoot));
    ASSERT_TRUE(foreign.Commit(&foreignRoot, inherited));
    ASSERT_TRUE(foreign.PrepareChild(foreignRoot.Handle(), harkentest::ObjectId(32), owner, 1, &foreignChild));
    harken::ActionFiringTable::PreparedChildIdentity issuer, foreignIssuer;
    ASSERT_TRUE(e.actionFirings.CapturePreparedChildIdentity(child, 1, &issuer));
    ASSERT_TRUE(foreign.CapturePreparedChildIdentity(foreignChild, 1, &foreignIssuer));
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::PreparedVariableWriteToken write;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.5f, observed, 0, &write).result);
    EXPECT_FALSE(e.variables.BindPreparedActionWriteOrder(write, e, foreignIssuer));
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(write, e, issuer));
    harken::VariableReadCutRef cut{}, refused{};
    EXPECT_FALSE(e.variables.CaptureActionReadCut(e, 0, 4 * kBlockFrames, foreignIssuer, 1, &refused));
    EXPECT_FALSE(refused.Valid());
    ASSERT_TRUE(e.variables.CaptureActionReadCut(e, 0, 4 * kBlockFrames, issuer, 1, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    EXPECT_FALSE(e.variables.PrepareActionReadCutSources(e, cut, foreignIssuer));
    ASSERT_TRUE(e.variables.PrepareActionReadCutSources(e, cut, issuer));
    EXPECT_FALSE(e.variables.SealPreparedActionSources(e, foreignIssuer));
    ASSERT_TRUE(e.variables.SealPreparedActionSources(e, issuer));
    ASSERT_TRUE(e.actionFirings.BumpCancellationEpoch(root.Handle()));
    EXPECT_FALSE(e.variables.BindPreparedActionWriteOrder(write, e, issuer));
    EXPECT_FALSE(e.variables.PrepareActionReadCutSources(e, cut, issuer));
    EXPECT_FALSE(e.variables.SealPreparedActionSources(e, issuer));
    EXPECT_FALSE(e.variables.ReadCutValue(cut, kVariable, &value));
    EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    e.variables.CancelPreparedWrite(&write);
    ASSERT_FALSE(write.Valid());
    ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
    ASSERT_TRUE(e.actionFirings.Abandon(&child));
    ASSERT_TRUE(foreign.Abandon(&foreignChild));
    EXPECT_EQ(0u, e.variables.AcceptedCount());
}

TEST(VariableCurrentSlice, DeferredActivatedThenCancelledReceiptCannotPublishPrivateWrite) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::PreparedActionFiring root, child;
    const auto owner = harken::ActionFiringOwnerStamp::DirectWorld(e.variables.CurrentWorldGeneration());
    ASSERT_TRUE(e.actionFirings.PrepareRoot(harkentest::ObjectId(31), owner, 1, &root));
    ASSERT_TRUE(e.actionFirings.Commit(&root, e.logicCommits.Issue()));
    ASSERT_TRUE(e.actionFirings.PrepareChild(root.Handle(), harkentest::ObjectId(32), owner, 1, &child));
    harken::ActionFiringTable::PreparedChildIdentity issuer;
    ASSERT_TRUE(e.actionFirings.CapturePreparedChildIdentity(child, 1, &issuer));
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::PreparedVariableWriteToken write;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.5f, observed, 0, &write).result);
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(write, e, issuer));
    harken::ActionFiringTable::ChildActivationReceipt receipt;
    ASSERT_TRUE(e.actionFirings.CommitPreparedChild(&child, &receipt));
    ASSERT_TRUE(e.actionFirings.BumpCancellationEpoch(root.Handle()));
    e.variables.CommitPreparedActionWrite(&write, receipt);
    EXPECT_TRUE(write.Valid());
    EXPECT_EQ(0u, e.variables.AcceptedCount());
    e.variables.CancelPreparedWrite(&write);
    EXPECT_FALSE(write.Valid());
}

TEST(VariableCurrentSlice, ActionReadCutIgnoresUnrelatedPendingBusClaimButKeepsRequiredSourcesBlocked) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    const auto other = harkentest::ObjectId(7002);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallSmoothDefinitions(&e, {kVariable, other}, 0, 1).status);
    RegisterLinearBinding(harkentest::ObjectId(7102), other, VN_TARGET_SCOPE_BUS,
        harken::kBusTargetBusVolume, 0, -12);
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(7102), &attach));
    ASSERT_EQ(VN_OK, Vn_Update());
    e.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto bus = e.buses.FindIndex(VN_BUS_MASTER);
    const auto revision = e.busTargets.TargetRevision(bus, harken::kBusTargetBusVolume);
    harken::VariableSliceRef blocker{};
    ASSERT_TRUE(e.variableSlices.Reserve(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), 0xD00D,
        e.mixer.BlocksRendered() * kBlockFrames, 0xD00D, 1, &blocker));
    ASSERT_TRUE(e.busTargets.TryClaim(bus, harken::kBusTargetBusVolume, blocker, revision));
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    observed.requestedEffectiveFrame = 8 * kBlockFrames;
    ASSERT_EQ(VN_OK, e.variables.AcceptGlobal(other, 1, observed,
        e.params, e.bindings, e.busTargets).result);
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    harken::PreparedVariableWriteToken write;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.5f, observed, 0, &write).result);
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(write, issue));
    harken::VariableReadCutRef independent{}, required{};
    float value = 0;
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 12 * kBlockFrames, issue, 1, &independent));
    ASSERT_TRUE(e.variables.ReadCutValue(independent, kVariable, &value));
    EXPECT_FLOAT_EQ(0.5f, value);
    EXPECT_TRUE(e.variables.PrepareActionReadCutSources(e, independent, issue));
    EXPECT_EQ(1u, e.variables.PendingCount());
    EXPECT_TRUE(e.busTargets.ForeignClaim(bus, harken::kBusTargetBusVolume));
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 12 * kBlockFrames, issue, 1, &required));
    ASSERT_TRUE(e.variables.ReadCutValue(required, other, &value));
    EXPECT_FALSE(e.variables.PrepareActionReadCutSources(e, required, issue));
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting,
        e.variables.PublishReadCut(independent, 12 * kBlockFrames));
    ASSERT_TRUE(e.variables.SealPreparedActionSources(issue));
    auto consumed = e.logicCommits.ConsumePreparedIssue(std::move(issue));
    e.variables.CommitPreparedActionWrite(&write, consumed);
    e.variables.CommitPreparedActionSources(consumed);
    e.variables.PublishPreparedPlaybackPages();
    for (std::uint32_t step = 0; step < 24 &&
        e.variables.ReadCutSourcesStatus(independent) == harken::VariableReadCutStatus::Waiting;
        ++step) e.nullBackend.Pump(1);
    EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(independent));
    EXPECT_TRUE(e.variables.ReleaseReadCut(independent));
    EXPECT_TRUE(e.variables.ReleaseReadCut(required));
    e.busTargets.ReleaseClaim(bus, harken::kBusTargetBusVolume, blocker);
    EXPECT_TRUE(e.variableSlices.AbandonPrepared(blocker));
}

void CheckActionPreparedPhysicalPrefix(bool cancel, bool reverseOrdinal) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    const auto other = harkentest::ObjectId(7002);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallSmoothDefinitions(&e, {kVariable, other}, 0, 1).status);
    RegisterBusVolumeBinding(harkentest::ObjectId(7101), VN_BUS_MASTER, 0, 0, 1, -6);
    RegisterLinearBinding(harkentest::ObjectId(7102), other, VN_TARGET_SCOPE_BUS,
        harken::kBusTargetBusVolume, 0, -12);
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(7102), &attach));
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto historyFree = harken::detail::VariableServiceTestAccess::OutcomeCapacity(e.variables)[2];
    const auto sliceFree = e.variableSlices.FreeRowCount();
    const auto targetFree = e.variableSlices.FreeTargetCapacity();
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    observed.requestedEffectiveFrame = 8 * kBlockFrames;
    harken::PreparedVariableWriteToken a, b;
    const auto ordinalA = reverseOrdinal ? 3u : 0u;
    constexpr auto ordinalB = 1u;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 1, observed, ordinalA, &a).result);
    observed.requestedEffectiveFrame = 12 * kBlockFrames;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(other, 1, observed, ordinalB, &b).result);
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(a, issue));
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(b, issue));
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 12 * kBlockFrames, issue, 4, &cut));
    float value = 0;
    // Only B is directly observed; its physical target also depends on A.
    ASSERT_TRUE(e.variables.ReadCutValue(cut, other, &value));
    ASSERT_TRUE(e.variables.PrepareActionReadCutSources(e, cut, issue));
    const auto sourceB = harken::detail::VariableServiceTestAccess::PreparedSliceForOrder(e.variables, issue.Sequence(), ordinalB);
    ASSERT_TRUE(sourceB.Valid());
    ASSERT_EQ(1u, e.variableSlices.TargetCount(sourceB));
    EXPECT_FLOAT_EQ(-18, e.variableSlices.Target(sourceB, 0)->resolvedValue);
    ASSERT_EQ(1u, e.variableSlices.SourceCount(sourceB));
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 12 * kBlockFrames));
    ASSERT_TRUE(e.variables.SealPreparedActionSources(issue));
    if (cancel) {
        e.variables.CancelPreparedWrite(&a);
        EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.ReadCutSourcesStatus(cut));
        EXPECT_FALSE(e.variables.SealPreparedActionSources(issue));
        e.variables.CancelPreparedWrite(&b);
        ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
        EXPECT_EQ(historyFree, harken::detail::VariableServiceTestAccess::OutcomeCapacity(e.variables)[2]);
        EXPECT_EQ(sliceFree, e.variableSlices.FreeRowCount());
        EXPECT_EQ(targetFree, e.variableSlices.FreeTargetCapacity());
        EXPECT_EQ(0u, e.variables.AcceptedCount());
    } else {
        auto consumed = e.logicCommits.ConsumePreparedIssue(std::move(issue));
        if (reverseOrdinal) {
            e.variables.CommitPreparedActionWrite(&b, consumed);
            e.variables.CommitPreparedActionWrite(&a, consumed);
        } else {
            e.variables.CommitPreparedActionWrite(&a, consumed);
            e.variables.CommitPreparedActionWrite(&b, consumed);
        }
        e.variables.CommitPreparedActionSources(consumed);
        e.variables.PublishPreparedPlaybackPages();
        for (std::uint32_t step = 0; step < 24 &&
            e.variables.ReadCutSourcesStatus(cut) == harken::VariableReadCutStatus::Waiting; ++step) e.nullBackend.Pump(1);
        EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(cut));
        EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
    }
}

TEST(VariableCurrentSlice, ActionPreparedSourceCombinesItsWholePrivatePhysicalPrefix) {
    CheckActionPreparedPhysicalPrefix(false, false);
}

TEST(VariableCurrentSlice, ActionPreparedSourcesPublishByActualFrameRatherThanOrdinal) {
    CheckActionPreparedPhysicalPrefix(false, true);
}

TEST(VariableCurrentSlice, ActionPreparedIndirectSourceCancellationFailsCutAndReclaimsPrivateStorage) {
    CheckActionPreparedPhysicalPrefix(true, false);
}

void CheckPrivatePreparationBeyond256(bool cancel) {
    using Access = harken::detail::VariableServiceTestAccess;
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    constexpr std::uint32_t count = 320;
    const auto freeBefore = Access::OutcomeCapacity(e.variables)[2];
    const auto proofsBefore = e.variableSlices.FreeRowCount();
    const auto payloadsBefore = e.variableSlices.FreePayloadCount();
    const auto targetsBefore = e.variableSlices.FreeTargetCapacity();
    const auto holdsBefore = e.timelinePool.CapturePrivateMaterializationSnapshotForTest().publicationHolds;
    std::array<harken::PreparedVariableWriteToken, count> writes{};
    std::array<harken::VariableSliceRef, count> slices{};
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    const auto sequence = issue.Sequence();
    for (std::uint32_t i = 0; i < count; ++i) {
        observed.requestedEffectiveFrame = static_cast<std::uint64_t>(8 + i / 64) * kBlockFrames;
        ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable,
            static_cast<float>(i + 1) / count, observed, i, &writes[i]).result);
        ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(writes[i], issue));
    }
    ASSERT_EQ(count, Access::PreparedCount(e.variables));
    ASSERT_GE(Access::HistorySlotForOrder(e.variables, sequence, count - 1), 256u);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 16 * kBlockFrames, issue, count, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    EXPECT_FLOAT_EQ(1.0f, value);
    ASSERT_TRUE(e.variables.PrepareActionReadCutSources(e, cut, issue));
    ASSERT_EQ(count, Access::HeldPlacementCount(e.variables));
    ASSERT_EQ(proofsBefore - count, e.variableSlices.FreeRowCount());
    ASSERT_EQ(payloadsBefore - count, e.variableSlices.FreePayloadCount());
    for (std::uint32_t i = 0; i < count; ++i) {
        slices[i] = Access::PreparedSliceForOrder(e.variables, sequence, i);
        ASSERT_TRUE(slices[i].Valid());
        ASSERT_EQ(harken::VariableSliceState::Prepared, e.variableSlices.State(slices[i]));
        for (std::uint32_t j = 0; j < i; ++j) EXPECT_NE(slices[i].slot, slices[j].slot);
    }
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 16 * kBlockFrames));
    ASSERT_TRUE(e.variables.SealPreparedActionSources(issue));
    harkentest::VariableOutcomeCapture outcomes(e);
    if (cancel) {
        for (auto& write : writes) {
            e.variables.CancelPreparedWrite(&write);
            EXPECT_FALSE(write.Valid());
        }
        EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.ReadCutSourcesStatus(cut));
        ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
        e.CompleteVariableOperation();
        EXPECT_EQ(0u, e.variables.AcceptedCount());
        EXPECT_EQ(0u, outcomes.Count());
    } else {
        auto consumed = e.logicCommits.ConsumePreparedIssue(std::move(issue));
        for (auto& write : writes) {
            e.variables.CommitPreparedActionWrite(&write, consumed);
            EXPECT_FALSE(write.Valid());
        }
        e.variables.CommitPreparedActionSources(consumed);
        e.variables.PublishPreparedPlaybackPages();
        EXPECT_EQ(0u, Access::HeldPlacementCount(e.variables));
        for (unsigned quantum = 0; quantum < 32 && outcomes.Count() < count; ++quantum) {
            e.nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        ASSERT_EQ(count, outcomes.Count());
        std::vector<std::uint64_t> ordinals;
        harken::VariableOutcomeRecord outcome{};
        while (outcomes.Pop(&outcome)) {
            EXPECT_EQ(sequence, outcome.acceptedTicket.sequence);
            EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
            EXPECT_EQ(kVariable, outcome.variableId);
            ordinals.push_back(outcome.bankVersionStableOrdinal);
        }
        std::sort(ordinals.begin(), ordinals.end());
        EXPECT_EQ(ordinals.end(), std::adjacent_find(ordinals.begin(), ordinals.end()));
        EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(cut));
        ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
        e.nullBackend.Pump(2);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(count, e.variables.AppliedCount());
    }
    EXPECT_EQ(0u, e.variables.PendingCount());
    EXPECT_EQ(0u, Access::PreparedCount(e.variables));
    EXPECT_EQ(0u, Access::HeldPlacementCount(e.variables));
    EXPECT_EQ(freeBefore, Access::OutcomeCapacity(e.variables)[2]);
    EXPECT_EQ(proofsBefore, e.variableSlices.FreeRowCount());
    EXPECT_EQ(payloadsBefore, e.variableSlices.FreePayloadCount());
    EXPECT_EQ(targetsBefore, e.variableSlices.FreeTargetCapacity());
    EXPECT_EQ(holdsBefore, e.timelinePool.CapturePrivateMaterializationSnapshotForTest().publicationHolds);
    EXPECT_FALSE(outcomes.Overflowed());
}

TEST(VariableCurrentSlice, PrivatePreparationBeyond256SealsCommitsAndAppliesEveryMember) {
    CheckPrivatePreparationBeyond256(false);
}

TEST(VariableCurrentSlice, PrivatePreparationBeyond256CancellationReturnsEveryPrivateResource) {
    CheckPrivatePreparationBeyond256(true);
}

TEST(VariableCurrentSlice, HighSlotPrivateDependencyAloneRetainsAndReturnsItsRevisionCredit) {
    using Access = harken::detail::VariableServiceTestAccess;
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    const auto other = harkentest::ObjectId(7002);
    const auto filler = harkentest::ObjectId(7003);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallSmoothDefinitions(&e, {kVariable, other, filler}, 0, 1).status);
    RegisterBusVolumeBinding(harkentest::ObjectId(7101), VN_BUS_MASTER, 0, 0, 1, -6);
    RegisterLinearBinding(harkentest::ObjectId(7102), other, VN_TARGET_SCOPE_BUS,
        harken::kBusTargetBusVolume, 0, -12);
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(7102), &attach));
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto freeBefore = Access::OutcomeCapacity(e.variables)[2];
    const auto proofsBefore = e.variableSlices.FreeRowCount();
    const auto payloadsBefore = e.variableSlices.FreePayloadCount();
    const auto targetsBefore = e.variableSlices.FreeTargetCapacity();
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    constexpr std::uint32_t initialFill = 4200;
    const auto capacity = e.variables.PendingCapacity();
    ASSERT_GT(capacity, initialFill + 2);
    for (std::uint32_t i = 0; i < initialFill; ++i) {
        observed.requestedEffectiveFrame = static_cast<std::uint64_t>(4096u + i) * kBlockFrames;
        ASSERT_EQ(VN_OK, e.variables.AcceptGlobal(filler, 0.25f, observed,
            e.params, e.bindings, e.busTargets).result);
    }
    harken::PreparedVariableWriteToken a, b;
    observed.requestedEffectiveFrame = 8 * kBlockFrames;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 1, observed, 0, &a).result);
    observed.requestedEffectiveFrame = 12 * kBlockFrames;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(other, 1, observed, 1, &b).result);
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    const auto sequence = issue.Sequence();
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(a, issue));
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(b, issue));
    ASSERT_GE(Access::HistorySlotForOrder(e.variables, sequence, 0), 4096u);
    ASSERT_GE(Access::HistorySlotForOrder(e.variables, sequence, 1), 4096u);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 12 * kBlockFrames, issue, 2, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, other, &value));
    ASSERT_TRUE(e.variables.PrepareActionReadCutSources(e, cut, issue));
    ASSERT_EQ(2u, Access::HeldPlacementCount(e.variables));
    const auto sourceA = Access::PreparedSliceForOrder(e.variables, sequence, 0);
    const auto sourceB = Access::PreparedSliceForOrder(e.variables, sequence, 1);
    ASSERT_TRUE(sourceA.Valid());
    ASSERT_TRUE(sourceB.Valid());
    ASSERT_EQ(1u, e.variableSlices.TargetCount(sourceB));
    EXPECT_FLOAT_EQ(-18.0f, e.variableSlices.Target(sourceB, 0)->resolvedValue);
    ASSERT_EQ(1u, e.variableSlices.SourceCount(sourceB));
    std::uint32_t cursor = 0;
    const auto* edge = e.variableSlices.NextSource(sourceB, &cursor);
    ASSERT_NE(nullptr, edge);
    EXPECT_EQ(sourceA.slot, edge->slice.slot);
    EXPECT_EQ(sourceA.generation, edge->slice.generation);
    harken::VariableSliceSource proofA{};
    ASSERT_TRUE(e.variableSlices.CaptureSource(sourceA, &proofA));
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 12 * kBlockFrames));
    ASSERT_TRUE(e.variables.SealPreparedActionSources(issue));
    auto consumed = e.logicCommits.ConsumePreparedIssue(std::move(issue));
    e.variables.CommitPreparedActionWrite(&a, consumed);
    e.variables.CommitPreparedActionWrite(&b, consumed);
    e.variables.CommitPreparedActionSources(consumed);
    e.variables.PublishPreparedPlaybackPages();
    ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
    ASSERT_EQ(0u, Access::ObserversForOrder(e.variables, sequence, 0));
    ASSERT_EQ(0u, Access::ObserversForOrder(e.variables, sequence, 1));
    ASSERT_EQ(2u, e.variableSlices.SourceObserverCount(sourceA));
    harkentest::VariableOutcomeCapture outcomes(e);
    for (unsigned quantum = 0; quantum < 12; ++quantum) {
        e.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        const auto fence = e.variableSlices.AppliedBlockSequence(sourceA);
        if (fence != 0 && e.mixer.BlocksRendered() > fence &&
            e.variableSlices.TargetCount(sourceA) == 0) break;
    }
    ASSERT_TRUE(e.variableSlices.SourceApplied(proofA));
    ASSERT_NE(harken::VariableSliceState::SliceApplied, e.variableSlices.State(sourceB));
    ASSERT_EQ(0u, e.variableSlices.TargetCount(sourceA));
    ASSERT_EQ(payloadsBefore - 1, e.variableSlices.FreePayloadCount());
    ASSERT_EQ(2u, e.variableSlices.SourceObserverCount(sourceA));
    ASSERT_EQ(1u, outcomes.Count());
    for (std::uint32_t i = initialFill; i < capacity - 2; ++i) {
        observed.requestedEffectiveFrame = static_cast<std::uint64_t>(4096u + i) * kBlockFrames;
        ASSERT_EQ(VN_OK, e.variables.AcceptGlobal(filler, 0.25f, observed,
            e.params, e.bindings, e.busTargets).result);
    }
    e.CompleteVariableOperation();
    EXPECT_EQ(freeBefore - capacity, Access::OutcomeCapacity(e.variables)[2]);
    const auto issued = e.logicCommits.IssuedCount();
    observed.requestedEffectiveFrame = 20000ull * kBlockFrames;
    EXPECT_EQ(VN_VARIABLE_TRANSACTION_EXHAUSTED, e.variables.AcceptGlobal(filler, 0.5f, observed,
        e.params, e.bindings, e.busTargets).result);
    EXPECT_EQ(issued, e.logicCommits.IssuedCount());
    for (unsigned quantum = 0; quantum < 12 &&
        Access::OutcomeCapacity(e.variables)[2] != freeBefore - capacity + 2; ++quantum) {
        e.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(freeBefore - capacity + 2, Access::OutcomeCapacity(e.variables)[2]);
    EXPECT_FALSE(e.variableSlices.SourceApplied(proofA));
    ASSERT_EQ(2u, outcomes.Count());
    std::array<std::uint64_t, 2> ordinals{};
    for (auto& ordinal : ordinals) {
        harken::VariableOutcomeRecord outcome{};
        ASSERT_TRUE(outcomes.Pop(&outcome));
        EXPECT_EQ(sequence, outcome.acceptedTicket.sequence);
        EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
        ordinal = outcome.bankVersionStableOrdinal;
    }
    EXPECT_NE(ordinals[0], ordinals[1]);
    for (unsigned i = 0; i < 2; ++i) {
        observed.requestedEffectiveFrame += kBlockFrames;
        ASSERT_EQ(VN_OK, e.variables.AcceptGlobal(filler, 0.5f, observed,
            e.params, e.bindings, e.busTargets).result);
    }
    EXPECT_EQ(freeBefore - capacity, Access::OutcomeCapacity(e.variables)[2]);
    e.variables.TerminateSession();
    e.CompleteVariableOperation();
    EXPECT_EQ(0u, e.variables.PendingCount());
    EXPECT_EQ(0u, Access::TotalObservers(e.variables));
    EXPECT_EQ(freeBefore, Access::OutcomeCapacity(e.variables)[2]);
    EXPECT_EQ(proofsBefore, e.variableSlices.FreeRowCount());
    EXPECT_EQ(payloadsBefore, e.variableSlices.FreePayloadCount());
    EXPECT_EQ(targetsBefore, e.variableSlices.FreeTargetCapacity());
    EXPECT_FALSE(outcomes.Overflowed());
}

TEST(VariableCurrentSlice, ActionPreparedCommitRequiresConsumedOriginalIssuerEvenWhenSequenceIsReused) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    harken::PreparedVariableWriteToken oldWrite;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.25f, observed, 0, &oldWrite).result);
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(oldWrite, issue));
    harken::VariableReadCutRef oldCut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 4 * kBlockFrames, issue, 1, &oldCut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(oldCut, kVariable, &value));
    ASSERT_TRUE(e.variables.PrepareActionReadCutSources(e, oldCut, issue));
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(oldCut, 4 * kBlockFrames));
    ASSERT_TRUE(e.variables.SealPreparedActionSources(issue));
    const auto sequence = issue.Sequence();
    harken::LogicCommitCoordinator::ConsumedIssue empty;
    e.variables.CommitPreparedActionWrite(&oldWrite, empty);
    EXPECT_TRUE(oldWrite.Valid());
    e.variables.CommitPreparedWrite(&oldWrite, sequence);
    EXPECT_TRUE(oldWrite.Valid());
    issue.Release();
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    ASSERT_EQ(sequence, issue.Sequence());
    harken::PreparedVariableWriteToken newWrite;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(kVariable, 0.75f, observed, 0, &newWrite).result);
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(newWrite, issue));
    harken::VariableReadCutRef newCut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 4 * kBlockFrames, issue, 1, &newCut));
    ASSERT_TRUE(e.variables.ReadCutValue(newCut, kVariable, &value));
    ASSERT_FLOAT_EQ(0.75f, value);
    ASSERT_TRUE(e.variables.PrepareActionReadCutSources(e, newCut, issue));
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(newCut, 4 * kBlockFrames));
    ASSERT_TRUE(e.variables.SealPreparedActionSources(issue));
    auto consumed = e.logicCommits.ConsumePreparedIssue(std::move(issue));
    e.variables.CommitPreparedActionWrite(&oldWrite, consumed);
    EXPECT_TRUE(oldWrite.Valid());
    e.variables.CommitPreparedActionWrite(&newWrite, consumed);
    EXPECT_FALSE(newWrite.Valid());
    e.variables.CommitPreparedActionSources(consumed);
    EXPECT_EQ(1u, e.variables.AcceptedCount());
    e.variables.CancelPreparedWrite(&oldWrite);
    EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.ReadCutSourcesStatus(oldCut));
    EXPECT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.ReadCutSourcesStatus(newCut));
    ASSERT_TRUE(e.variables.ReleaseReadCut(oldCut));
    e.variables.PublishPreparedPlaybackPages();
    for (std::uint32_t step = 0; step < 12 &&
        e.variables.ReadCutSourcesStatus(newCut) == harken::VariableReadCutStatus::Waiting; ++step) e.nullBackend.Pump(1);
    EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(newCut));
    EXPECT_TRUE(e.variables.ReleaseReadCut(newCut));
}

TEST(VariableCurrentSlice, ReadCutRejectsChangedWinnerAfterThatWinnerHistoryWasReclaimed) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    observed.requestedEffectiveFrame = 2 * kBlockFrames;
    const auto first = e.variables.AcceptGlobal(kVariable, 0.25f, observed, e.params, e.bindings, e.busTargets);
    observed.requestedEffectiveFrame = kBlockFrames;
    const auto second = e.variables.AcceptGlobal(kVariable, 0.75f, observed, e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, first.result);
    ASSERT_EQ(VN_OK, second.result);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 8 * kBlockFrames, second.ticket.sequence + 1, 0, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    EXPECT_FLOAT_EQ(0.25f, value);
    e.variables.BeginService(e, 3, kBlockFrames);
    e.variables.PrepareClaimedSlices(e);
    const auto winner = e.variables.SliceForTicketForTests(second.ticket.sequence);
    e.nullBackend.Pump(12);
    e.variables.EndService(e);
    e.variables.MarkOutcomeDeliveryComplete();
    e.variables.FinishOutcomeDelivery();
    e.nullBackend.Pump(1);
    e.variables.EndService(e);
    EXPECT_EQ(harken::VariableSliceState::Free, e.variableSlices.State(winner));
    ASSERT_TRUE(e.variables.CurrentValue(kVariable, &value));
    EXPECT_FLOAT_EQ(0.75f, value);
    EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.PublishReadCut(cut, 8 * kBlockFrames));
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, CanceledObservedEmitterPreparationDoesNotReserveItsOldOverrideSlot) {
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.emitterVariableSlots = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    const auto other = harkentest::OffsetObjectId(kVariable, 1);
    const std::array definitions = {
        MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0, 1, 0),
        MakeVariableDefinition(other, harken::VariableKindV1::Smooth, 0, 1, 0)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, definitions.data(), 2, 0x45505250).status);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    harken::PreparedVariableWriteToken old, next;
    ASSERT_EQ(VN_OK, e.variables.PrepareEmitterWrite(emitter, kVariable, 0.5f, observed, 0,
        e.emitters, &old).result);
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(old, issue));
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(emitter, 4 * kBlockFrames, issue, 1, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    EXPECT_FLOAT_EQ(0.5f, value);
    e.variables.CancelPreparedWrite(&old);
    EXPECT_FALSE(e.variables.ReadCutValue(cut, kVariable, &value));
    EXPECT_EQ(VN_OK, e.variables.PrepareEmitterWrite(emitter, other, 0.75f, observed, 2,
        e.emitters, &next).result);
    e.variables.CancelPreparedWrite(&next);
    EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, ReadCutRejectsDestroyedEmitterEvenWithoutAnOverrideSource) {
    VnConfig config = CurrentSliceConfig(2, kBlockFrames, kSampleRate);
    config.maxEmitters = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(emitter, 4 * kBlockFrames, 1, 1, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    EXPECT_FLOAT_EQ(0.0f, value);
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    EXPECT_EQ(harken::VariableReadCutStatus::Failed,
        e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    harken::VariableReadCutRef stale{};
    EXPECT_FALSE(e.variables.CaptureReadCut(emitter, 4 * kBlockFrames, 1, 1, &stale));
    EXPECT_FALSE(stale.Valid());
    VnEmitterHandle replacement = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&replacement));
    EXPECT_NE(emitter, replacement);
    EXPECT_EQ(static_cast<std::uint32_t>(emitter), static_cast<std::uint32_t>(replacement));
    EXPECT_EQ(harken::VariableReadCutStatus::Failed,
        e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    EXPECT_FALSE(e.variables.CaptureReadCut(emitter, 4 * kBlockFrames, 1, 1, &stale));
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
    ASSERT_TRUE(e.variables.CaptureReadCut(replacement, 4 * kBlockFrames, 1, 1, &cut));
    EXPECT_EQ(harken::VariableReadCutStatus::Applied,
        e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, CapturedReaderPrimesCompleteBuiltinInputsAndPreservesLiveUpdates) {
    EngineSession session(CurrentSliceConfig(4, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const VnVoiceHandle firstVoice = std::uint64_t{1} << 32;
    const VnVoiceHandle secondVoice = firstVoice | 1u;
    struct TargetCase { std::uint32_t target; float atZero; float atOne; float expectedOne; };
    const TargetCase cases[] = {
        {harken::kTargetVoiceVolume, 0, -12, -12},
        {harken::kTargetVoicePitch, 0, 1200, 1200},
        {harken::kTargetVoiceLowpassCutoff, 8000, 2000, 2000},
        {harken::kTargetVoiceLowpassQ, 0, 1, 2 * harken::kToneDefaultQ},
        {harken::kTargetVoiceLowShelfGainDb, 0, -6, -6},
        {harken::kTargetVoiceLowShelfFreqHz, 0, 1, 2 * harken::kToneLowShelfDefaultHz},
        {harken::kTargetVoiceLowShelfQ, 0, 1, 2 * harken::kToneDefaultQ},
        {harken::kTargetVoicePan, 0, 0.5f, 0.5f}};
    for (const auto voice : {firstVoice, secondVoice}) {
        e.params.OnVoiceStarted(voice, 0, 1);
        for (const auto& target : cases) {
            ASSERT_TRUE(e.params.BindVoiceTarget(kVariable, voice, target.target,
                LinearCurve(0, target.atZero, 1, target.atOne)));
        }
    }
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    const auto first = e.variables.AcceptGlobal(kVariable, 1, observed, e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, first.result);
    harken::VariableReadCutRef firstCut{}, secondCut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, first.ticket.sequence + 1, 0, &firstCut));
    auto firstReader = e.variables.ReadCutReader(firstCut);
    float input = 0;
    ASSERT_EQ(harken::VariableReadCutValueStatus::Value, firstReader.Read(kVariable, &input));
    const auto second = e.variables.AcceptGlobal(kVariable, 0, observed, e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, second.result);
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, second.ticket.sequence + 1, 0, &secondCut));
    auto secondReader = e.variables.ReadCutReader(secondCut);
    EXPECT_EQ(harken::VariableReadCutValueStatus::Missing,
        firstReader.Read(harkentest::OffsetObjectId(kVariable, 1), &input));
    float db = 0;
    ASSERT_TRUE(e.params.TryPeekResolvedVolume(firstVoice, nullptr, &db, &firstReader));
    EXPECT_FLOAT_EQ(-12.0f, db);
    ASSERT_TRUE(e.params.TryPeekResolvedVolume(secondVoice, nullptr, &db, &secondReader));
    EXPECT_FLOAT_EQ(0.0f, db);
    std::array<harken::ParamSystem::PrimedValue, harken::kBuiltinTargetCount> primed{};
    std::uint32_t written = 0;
    ASSERT_TRUE(e.params.TryPrimeVoiceBindings(firstVoice, firstReader, primed.data(), primed.size(), &written));
    EXPECT_EQ(std::size(cases), written);
    for (const auto& target : cases) {
        EXPECT_NEAR(target.expectedOne, e.params.ResolvedValue(firstVoice, target.target), 1.0e-4f);
    }
    ASSERT_TRUE(e.params.TryPrimeVoiceBindings(secondVoice, secondReader, primed.data(), primed.size(), &written));
    EXPECT_FLOAT_EQ(0, e.params.ResolvedValue(secondVoice, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(8000, e.params.ResolvedValue(secondVoice, harken::kTargetVoiceLowpassCutoff));
    EXPECT_FLOAT_EQ(harken::kToneDefaultQ, e.params.ResolvedValue(secondVoice, harken::kTargetVoiceLowpassQ));
    float installed = 1;
    ASSERT_TRUE(e.params.ScopedVariableValue(nullptr, 0, kVariable, &installed));
    EXPECT_FLOAT_EQ(0, installed);
    std::vector<harken::ParamCommit> commits;
    e.params.FlushCommits([&](const auto& commit) {
        commits.push_back(commit);
        return harken::FlushOutcome::Committed;
    });
    EXPECT_FLOAT_EQ(0, e.params.ResolvedValue(firstVoice, harken::kTargetVoiceVolume));
    EXPECT_FALSE(commits.empty());
    EXPECT_TRUE(e.variables.ReleaseReadCut(firstCut));
    EXPECT_TRUE(e.variables.ReleaseReadCut(secondCut));
}

TEST(VariableCurrentSlice, CapturedReaderFailureDoesNotFallBackOrPartiallyPrimeTargets) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    const auto secondVariable = harkentest::OffsetObjectId(kVariable, 1);
    const std::array definitions = {
        MakeVariableDefinition(kVariable, harken::VariableKindV1::Smooth, 0, 1, 0),
        MakeVariableDefinition(secondVariable, harken::VariableKindV1::Smooth, 0, 1, 0)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, definitions.data(), 2, 0x43555446).status);
    const VnVoiceHandle voice = std::uint64_t{1} << 32;
    e.params.OnVoiceStarted(voice, 0, 1);
    ASSERT_TRUE(e.params.BindVoiceTarget(kVariable, voice, harken::kTargetVoiceVolume,
        LinearCurve(0, 0, 1, -12)));
    ASSERT_TRUE(e.params.BindVoiceTarget(secondVariable, voice, harken::kTargetVoiceLowpassCutoff,
        LinearCurve(0, 8000, 1, 2000)));
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    harken::PreparedVariableWriteToken canceled;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(secondVariable, 1, observed, 0, &canceled).result);
    harken::LogicCommitCoordinator::PreparedIssue issue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, e.logicCommits.PrepareIssue(&issue));
    ASSERT_TRUE(e.variables.BindPreparedActionWriteOrder(canceled, issue));
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureActionReadCut(0, 4 * kBlockFrames, issue, 1, &cut));
    auto reader = e.variables.ReadCutReader(cut);
    float input = 0;
    ASSERT_EQ(harken::VariableReadCutValueStatus::Value, reader.Read(secondVariable, &input));
    e.variables.CancelPreparedWrite(&canceled);
    const auto dirty = e.params.DirtyTargetCount();
    const auto volume = e.params.LastCommitted(voice, harken::kTargetVoiceVolume);
    const auto cutoff = e.params.LastCommitted(voice, harken::kTargetVoiceLowpassCutoff);
    std::array<harken::ParamSystem::PrimedValue, harken::kBuiltinTargetCount> primed{};
    std::uint32_t written = 17;
    EXPECT_FALSE(e.params.TryPrimeVoiceBindings(voice, reader, primed.data(), primed.size(), &written));
    EXPECT_EQ(0u, written);
    EXPECT_EQ(dirty, e.params.DirtyTargetCount());
    EXPECT_FLOAT_EQ(volume, e.params.LastCommitted(voice, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(cutoff, e.params.LastCommitted(voice, harken::kTargetVoiceLowpassCutoff));
    EXPECT_EQ(harken::VariableReadCutValueStatus::Failed, reader.Read(secondVariable, &input));
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
    EXPECT_EQ(harken::VariableReadCutValueStatus::Failed, reader.Read(kVariable, &input));
}

TEST(VariableCurrentSlice, CapturedReaderRejectsInvalidatedPublishedSourceBeforeServiceFinalizesIt) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const VnVoiceHandle voice = std::uint64_t{1} << 32;
    e.params.OnVoiceStarted(voice, 0, 1);
    ASSERT_TRUE(e.params.BindVoiceTarget(kVariable, voice, harken::kTargetVoiceVolume,
        LinearCurve(0, 0, 1, -12)));
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    const auto accepted = e.variables.AcceptGlobal(kVariable, 1, observed, e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, accepted.result);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, accepted.ticket.sequence + 1, 0, &cut));
    auto reader = e.variables.ReadCutReader(cut);
    float value = 0;
    ASSERT_EQ(harken::VariableReadCutValueStatus::Value, reader.Read(kVariable, &value));
    e.variables.BeginService(e, 0, kBlockFrames);
    e.variables.PrepareClaimedSlices(e);
    const auto source = e.variables.SliceForTicketForTests(accepted.ticket.sequence);
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(source));
    ASSERT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim,
        e.variableSlices.Invalidate(source, 1));
    const auto dirty = e.params.DirtyTargetCount();
    const auto committed = e.params.LastCommitted(voice, harken::kTargetVoiceVolume);
    std::array<harken::ParamSystem::PrimedValue, harken::kBuiltinTargetCount> primed{};
    std::uint32_t written = 17;
    EXPECT_EQ(harken::VariableReadCutValueStatus::Failed, reader.Read(kVariable, &value));
    EXPECT_FALSE(e.params.TryPrimeVoiceBindings(voice, reader, primed.data(), primed.size(), &written));
    EXPECT_EQ(0u, written);
    EXPECT_EQ(dirty, e.params.DirtyTargetCount());
    EXPECT_FLOAT_EQ(committed, e.params.LastCommitted(voice, harken::kTargetVoiceVolume));
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, NativeReadCutCalendarRequiresExactConsumedIdentityInEitherReceiptOrder) {
    for (const bool failedFirst : {false, true}) {
        SCOPED_TRACE(failedFirst);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        harken::VariableReadCutRef cut{};
        constexpr auto frame = 4 * kBlockFrames;
        ASSERT_TRUE(e.variables.CaptureReadCut(0, frame, 7, 3, &cut));
        harken::HeldCurrentSliceToken calendar;
        ASSERT_TRUE(e.variables.ReserveNativeReadCutCalendar(cut, 0, &calendar));
        EXPECT_FALSE(e.variables.ReleaseReadCut(cut));
        ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, frame));
        harken::VariableNativeStartIdentity identity{};
        ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, std::uint64_t{1} << 32, frame, &identity));
        ASSERT_TRUE(e.variables.CommitNativeReadCut(identity));
        EXPECT_FALSE(e.variables.CancelNativeReadCutCalendar(cut, &calendar));
        ASSERT_TRUE(e.timelinePool.ReleaseHeldCurrentSlice(&calendar));
        if (failedFirst) {
            ASSERT_TRUE(e.variables.FinishNativeReadCut(identity, false));
            EXPECT_FALSE(e.variables.AcknowledgeNativeReadCut(identity));
        } else {
            ASSERT_TRUE(e.variables.MarkNativeReadCutInstalled(identity));
            EXPECT_FALSE(e.variables.FinishNativeReadCut(identity, true));
        }
        e.timelinePool.Publish(frame + kBlockFrames);
        harken::ConsumedCurrentSlice consumed{};
        ASSERT_EQ(1u, e.timelinePool.ConsumeCurrentSlicesRt(4, &consumed, 1));
        for (std::uint32_t field = 0; field < 6; ++field) {
            auto wrong = consumed;
            if (field == 0) ++wrong.executeFrame;
            if (field == 1) ++wrong.logicCommitSequence;
            if (field == 2) ++wrong.executionOrdinal;
            if (field == 3) ++wrong.slice.generation;
            if (field == 4) wrong.slice.slot = UINT32_MAX;
            if (field == 5) wrong.kind = harken::CurrentSliceKind::VariableBankCut;
            EXPECT_FALSE(e.variables.ConsumeNativeReadCutCalendar(wrong));
            EXPECT_FALSE(e.variables.NativeReadCutCalendarConsumed(identity));
        }
        ASSERT_TRUE(e.variables.ConsumeNativeReadCutCalendar(consumed));
        EXPECT_TRUE(e.variables.NativeReadCutCalendarConsumed(identity));
        if (!failedFirst) ASSERT_TRUE(e.variables.FinishNativeReadCut(identity, true));
        ASSERT_TRUE(e.variables.AcknowledgeNativeReadCut(identity));
        EXPECT_FALSE(e.variables.ConsumeNativeReadCutCalendar(consumed));
        EXPECT_FALSE(e.variables.HasNativeReadCutsForTransport(identity.transport));
    }
}

TEST(VariableCurrentSlice, MixerConsumesNativeCalendarsOnlyAfterTheActualPagePrefix) {
    for (const bool partial : {false, true}) {
        SCOPED_TRACE(partial);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        e.nullBackend.Pump(1);
        const auto firstQuantum = e.timelinePool.ConsumerFrontier().load();
        const auto firstFrame = e.timelinePool.PublishCutoffFrame();
        ASSERT_EQ(firstQuantum * kBlockFrames, firstFrame);
        std::array<harken::VariableNativeStartIdentity, 2> identities{};
        for (std::uint32_t index = 0; index < identities.size(); ++index) {
            const auto frame = firstFrame + index * kBlockFrames;
            harken::VariableReadCutRef cut{};
            ASSERT_TRUE(e.variables.CaptureReadCut(0, frame, index + 1u, 0, &cut));
            harken::HeldCurrentSliceToken held;
            ASSERT_TRUE(e.variables.ReserveNativeReadCutCalendar(cut, firstFrame, &held));
            ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, frame));
            ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, (std::uint64_t{1} << 32) + index,
                frame, &identities[index]));
            ASSERT_TRUE(e.variables.CommitNativeReadCut(identities[index]));
            ASSERT_TRUE(e.variables.MarkNativeReadCutInstalled(identities[index]));
            ASSERT_TRUE(e.timelinePool.ReleaseHeldCurrentSlice(&held));
        }
        e.timelinePool.Publish(firstFrame + 2 * kBlockFrames);

        e.mixer.ArenaForTests().SetCapacityForTests(0);
        e.nullBackend.Pump(1);
        e.mixer.ArenaForTests().RestoreCapacityForTests();
        EXPECT_EQ(firstQuantum, e.timelinePool.ConsumerFrontier().load());
        for (const auto& identity : identities) {
            EXPECT_FALSE(e.variables.NativeReadCutCalendarConsumed(identity));
            EXPECT_FALSE(e.variables.FinishNativeReadCut(identity, true));
        }

        if (partial) e.mixer.SetCurrentSliceScratchLimitForTest(1);
        CurrentSlicePrefixObservation observation{};
        e.mixer.SetCurrentSliceFrontierHookForTest(ObserveCurrentSlicePrefix, &observation);
        e.nullBackend.Pump(1);
        e.mixer.SetCurrentSliceFrontierHookForTest(nullptr, nullptr);
        e.mixer.RestoreCurrentSliceScratchLimitForTest();
        const auto consumedPages = partial ? 1u : 2u;
        EXPECT_EQ(consumedPages, observation.stagedPageCount);
        EXPECT_EQ(firstQuantum + consumedPages, e.timelinePool.ConsumerFrontier().load());
        EXPECT_TRUE(e.variables.NativeReadCutCalendarConsumed(identities[0]));
        EXPECT_EQ(!partial, e.variables.NativeReadCutCalendarConsumed(identities[1]));
        if (partial) {
            EXPECT_FALSE(e.variables.FinishNativeReadCut(identities[1], true));
            e.nullBackend.Pump(1);
            EXPECT_TRUE(e.variables.NativeReadCutCalendarConsumed(identities[1]));
        }
        for (const auto& identity : identities) {
            EXPECT_TRUE(e.variables.FinishNativeReadCut(identity, true));
            EXPECT_TRUE(e.variables.AcknowledgeNativeReadCut(identity));
        }
    }
}

TEST(VariableCurrentSlice, NativeReadCutCalendarWithdrawalRequiresOwnHeldToken) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::VariableReadCutRef first{}, second{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 1, 1, &first));
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 2, 1, &second));
    harken::HeldCurrentSliceToken a, b;
    ASSERT_TRUE(e.variables.ReserveNativeReadCutCalendar(first, 0, &a));
    ASSERT_TRUE(e.variables.ReserveNativeReadCutCalendar(second, 0, &b));
    EXPECT_FALSE(e.variables.CancelNativeReadCutCalendar(first, &b));
    EXPECT_FALSE(e.variables.ReleaseReadCut(first));
    EXPECT_FALSE(e.variables.ReleaseReadCut(second));
    ASSERT_TRUE(e.variables.CancelNativeReadCutCalendar(first, &a));
    ASSERT_TRUE(e.variables.ReleaseReadCut(first));
    EXPECT_TRUE(b.IsHeld());
    ASSERT_TRUE(e.variables.CancelNativeReadCutCalendar(second, &b));
    ASSERT_TRUE(e.variables.ReleaseReadCut(second));
    EXPECT_FALSE(e.variables.HasNativeReadCutsForTransport(e.variables.CurrentTransportEpoch()));
}

TEST(VariableCurrentSlice, NativeVoiceWaitsForFullFifoInstallationAndCalendarInEitherOrder) {
    for (const bool calendarFirst : {false, true}) {
        SCOPED_TRACE(calendarFirst);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        const std::vector<float> samples(kSampleRate, 0.05f);
        const auto pcm = MonoDesc(samples, kSampleRate);
        const auto sound = harkentest::ObjectId(7101);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
        const auto view = e.registry.Lookup(sound);
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, e.voices.Play(view, 0, 1, 0, false, 0, 0, 0,
            harken::ResolvePanGains(e.config.mixChannelLayout, 0), 0, view.frames, 0,
            harken::SelectResampleProfile(kSampleRate, kSampleRate), &voice,
            e.buses.GenerationAt(0), UINT32_MAX, 0, true));
        constexpr auto frame = 4 * kBlockFrames;
        harken::VariableReadCutRef cut{};
        ASSERT_TRUE(e.variables.CaptureReadCut(0, frame, 1, 0, &cut));
        harken::HeldCurrentSliceToken held;
        ASSERT_TRUE(e.variables.ReserveNativeReadCutCalendar(cut, 0, &held));
        ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, frame));
        harken::VariableNativeStartIdentity identity{};
        ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, voice, frame, &identity));
        ASSERT_TRUE(e.voices.BindPreparedNativeReadCut(voice, identity));
        EXPECT_FALSE(e.voices.BindPreparedNativeReadCut(voice, identity));
        ASSERT_TRUE(e.variables.CommitNativeReadCut(identity));
        ASSERT_TRUE(e.voices.CommitPreparedVoice(voice));
        EXPECT_FALSE(e.voices.CommitPreparedVoice(voice));
        EXPECT_FALSE(e.voices.AbortClaimedVoice(voice));
        ASSERT_TRUE(e.timelinePool.ReleaseHeldCurrentSlice(&held));
        e.timelinePool.Publish(frame + kBlockFrames);
        const auto silent = [&] {
            const float* output = e.nullBackend.LastBlock();
            for (std::uint32_t i = 0; i < 2 * kBlockFrames; ++i) EXPECT_FLOAT_EQ(0, output[i]);
        };
        if (calendarFirst) {
            while (e.mixer.BlocksRendered() <= 4) {
                e.nullBackend.Pump(1);
                silent();
            }
            EXPECT_TRUE(e.variables.NativeReadCutCalendarConsumed(identity));
            EXPECT_EQ(harken::VariableNativeStartReceipt::Pending, e.variables.NativeReadCutStatus(identity));
        }
        std::array<harken::CommandRecord, 2> commands{};
        commands[0].opcode = harken::kOpcodePlay;
        commands[0].payload.play.handle = voice;
        commands[0].payload.play.sourceIdentity = voice;
        commands[0].payload.play.batch = {harken::kPlayLedBatchVersion,
            harken::PlayLedBatchKind::DeferredStart, 0, 2, 0};
        commands[1].opcode = harken::kOpcodeSetVoiceVolume;
        commands[1].payload.setVoiceVolume.handle = voice;
        commands[1].payload.setVoiceVolume.volumeDb = -12;
        harken::CommandRecord prefix{};
        prefix.opcode = harken::kOpcodeSetVoiceVolume;
        prefix.payload.setVoiceVolume.handle = 0;
        ASSERT_TRUE(e.commands.TryPush(prefix));
        ASSERT_TRUE(e.commands.TryPushBatch(commands.data(), commands.size()));
        const auto ceiling = e.mixer.MaxCommandsPerQuantum();
        e.mixer.SetMaxCommandsPerQuantumForTest(2);
        e.nullBackend.Pump(1);
        silent();
        EXPECT_EQ(harken::VariableNativeStartReceipt::Pending, e.variables.NativeReadCutStatus(identity));
        e.mixer.SetMaxCommandsPerQuantumForTest(ceiling);
        e.nullBackend.Pump(1);
        if (!calendarFirst) {
            silent();
            EXPECT_EQ(harken::VariableNativeStartReceipt::Installed, e.variables.NativeReadCutStatus(identity));
            while (e.mixer.BlocksRendered() <= 4) e.nullBackend.Pump(1);
        }
        EXPECT_EQ(harken::VariableNativeStartReceipt::Activated, e.variables.NativeReadCutStatus(identity));
        bool produced = false;
        const float* output = e.nullBackend.LastBlock();
        for (std::uint32_t i = 0; i < kBlockFrames; ++i) {
            if (std::abs(output[i * 2]) <= 1e-7f) continue;
            produced = true;
            EXPECT_NEAR(0.05f * DbToLinear(-12), output[i * 2], 1e-5f);
        }
        EXPECT_TRUE(produced);
        ASSERT_TRUE(e.variables.AcknowledgeNativeReadCut(identity));
        e.nullBackend.Pump(1);
        EXPECT_NE(nullptr, e.voices.Resolve(voice));
    }
}

TEST(VariableCurrentSlice, NativeVoiceSourceFailureAndStopReleaseOnlyAfterCalendarConsumption) {
    for (const int failure : {0, 1, 2, 3, 4, 5}) {
        SCOPED_TRACE(failure);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
            e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
        const auto accepted = e.variables.AcceptGlobal(kVariable, 1, observed, e.params, e.bindings, e.busTargets);
        ASSERT_EQ(VN_OK, accepted.result);
        const std::vector<float> samples(kSampleRate, 0.05f);
        const auto pcm = MonoDesc(samples, kSampleRate);
        const auto sound = harkentest::ObjectId(7102);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
        const auto view = e.registry.Lookup(sound);
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, e.voices.Play(view, 0, 1, 0, false, 0, 0, 0,
            harken::ResolvePanGains(e.config.mixChannelLayout, 0), 0, view.frames, 0,
            harken::SelectResampleProfile(kSampleRate, kSampleRate), &voice,
            e.buses.GenerationAt(0), UINT32_MAX, 0, true));
        constexpr auto frame = 4 * kBlockFrames;
        harken::VariableReadCutRef cut{};
        ASSERT_TRUE(e.variables.CaptureReadCut(0, frame, accepted.ticket.sequence + 1, 0, &cut));
        float value = 0;
        ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
        harken::HeldCurrentSliceToken held;
        ASSERT_TRUE(e.variables.ReserveNativeReadCutCalendar(cut, 0, &held));
        e.variables.BeginService(e, 0, kBlockFrames);
        e.variables.PrepareClaimedSlices(e);
        ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, frame));
        harken::VariableNativeStartIdentity identity{};
        ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, voice, frame, &identity));
        ASSERT_TRUE(e.voices.BindPreparedNativeReadCut(voice, identity));
        ASSERT_TRUE(e.variables.CommitNativeReadCut(identity));
        ASSERT_TRUE(e.voices.CommitPreparedVoice(voice));
        ASSERT_TRUE(e.timelinePool.ReleaseHeldCurrentSlice(&held));
        e.timelinePool.Publish(frame + kBlockFrames);
        harken::CommandRecord command{};
        command.opcode = harken::kOpcodePlay;
        command.payload.play.handle = voice;
        command.payload.play.sourceIdentity = voice;
        command.payload.play.batch = {harken::kPlayLedBatchVersion,
            harken::PlayLedBatchKind::DirectStart, 0, 1, 0};
        if (failure >= 2) {
            command.payload.play.batch.batchKind = harken::PlayLedBatchKind::None;
            command.payload.play.batch.reserved = failure == 3 ? 1 : 0;
            if (failure >= 4) {
                command.payload.play.batch.batchKind = failure == 4
                    ? harken::PlayLedBatchKind::TimelineArm : harken::PlayLedBatchKind::TimelineControlArm;
                command.payload.play.batch.armToken = 1;
            }
        }
        ASSERT_TRUE(e.commands.TryPush(command));
        if (failure == 1) {
            command = {};
            command.opcode = harken::kOpcodeStop;
            command.payload.stop.handle = voice;
            command.payload.stop.fadeMs = 20;
            ASSERT_TRUE(e.commands.TryPush(command));
        } else if (failure == 0) {
            const auto source = e.variables.SliceForTicketForTests(accepted.ticket.sequence);
            ASSERT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim,
                e.variableSlices.Invalidate(source, 1));
        }
        const auto violations = e.mixer.CommandProtocolViolationCount();
        e.mixer.SuppressCommandProtocolAssertForTest(failure >= 2);
        e.nullBackend.Pump(1);
        e.mixer.SuppressCommandProtocolAssertForTest(false);
        EXPECT_EQ(violations + (failure >= 2 ? 1 : 0), e.mixer.CommandProtocolViolationCount());
        EXPECT_EQ(harken::VariableNativeStartReceipt::Failed, e.variables.NativeReadCutStatus(identity));
        EXPECT_EQ(nullptr, e.voices.Resolve(voice));
        EXPECT_FALSE(e.variables.AcknowledgeNativeReadCut(identity));
        while (e.mixer.BlocksRendered() <= 4) {
            e.nullBackend.Pump(1);
            const float* output = e.nullBackend.LastBlock();
            for (std::uint32_t i = 0; i < 2 * kBlockFrames; ++i) EXPECT_FLOAT_EQ(0, output[i]);
        }
        EXPECT_TRUE(e.variables.NativeReadCutCalendarConsumed(identity));
        EXPECT_TRUE(e.variables.AcknowledgeNativeReadCut(identity));
    }
}

TEST(VariableCurrentSlice, ConsumedSlicesWaitForFifoWithoutLosingDependencyOrder) {
    for (const bool cancelSource : {false, true}) {
        SCOPED_TRACE(cancelSource);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        const auto publicVoice = PlayDc(&e, harkentest::ObjectId(7108), VN_BUS_MASTER);
        const auto voice = harkentest::RenderHandle(&e, publicVoice);
        e.nullBackend.Pump(1);
        harken::CommandRecord queued{};
        queued.opcode = harken::kOpcodeSetVoiceVolume;
        queued.payload.setVoiceVolume.handle = voice;
        ASSERT_TRUE(e.commands.TryPush(queued));
        const auto required = e.commands.PublishedHead();
        e.mixer.SetMaxCommandsPerQuantumForTest(0);
        constexpr auto frame = 8 * kBlockFrames;
        harken::VariableSliceRef source{}, dependent{}, independent{};
        const auto reserve = [&](std::uint64_t sequence, std::uint32_t targets, harken::VariableSliceRef* out) {
            return e.variableSlices.Reserve(e.variables.CurrentWorldGeneration(),
                e.variables.CurrentTransportEpoch(), sequence, frame, sequence, targets, out);
        };
        ASSERT_TRUE(reserve(2, 1, &dependent));
        ASSERT_TRUE(reserve(1, 1, &source));
        ASSERT_TRUE(reserve(3, 0, &independent));
        for (const auto& entry : {std::pair{source, -12.0f}, std::pair{dependent, -6.0f}}) {
            harken::VariableSliceTarget target{};
            target.locator.scope = harken::VariableTargetScope::Voice;
            target.locator.object = voice;
            target.locator.objectGeneration = static_cast<std::uint32_t>(voice >> 32);
            target.locator.targetId = harken::kTargetVoiceVolume;
            target.targetRevision = 1;
            target.command.opcode = harken::kOpcodeSetVoiceVolume;
            target.command.payload.setVoiceVolume.handle = voice;
            target.command.payload.setVoiceVolume.volumeDb = entry.second;
            ASSERT_TRUE(e.variableSlices.AddTarget(entry.first, target));
        }
        ASSERT_TRUE(e.variableSlices.SetPreparedCommandFrontier(source, required));
        harken::VariableSliceSource proof{};
        ASSERT_TRUE(e.variableSlices.CaptureSource(source, &proof));
        ASSERT_TRUE(e.variableSlices.AddSource(dependent, proof));
        for (const auto slice : {source, dependent, independent}) {
            ASSERT_TRUE(e.variableSlices.Publish(slice));
            harken::ScheduledCurrentSliceInput input{};
            input.executeFrame = frame;
            input.logicCommitSequence = slice.slot == source.slot ? 1 : slice.slot == dependent.slot ? 2 : 3;
            input.slice = {slice.slot, slice.generation};
            harken::HeldCurrentSliceToken held;
            ASSERT_EQ(harken::ReserveStatus::Committed,
                e.timelinePool.ReserveHeldCurrentSlice(input, harken::PrePublishLatePolicy::Refuse, 0, &held).status);
            ASSERT_TRUE(e.timelinePool.ReleaseHeldCurrentSlice(&held));
        }
        e.timelinePool.Publish(frame + kBlockFrames);
        while (e.mixer.BlocksRendered() <= 8) e.nullBackend.Pump(1);
        EXPECT_LT(e.commands.SettledFrontier(), required);
        EXPECT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(source));
        EXPECT_EQ(harken::VariableSliceState::PageCommitted, e.variableSlices.State(dependent));
        EXPECT_EQ(harken::VariableSliceState::SliceApplied, e.variableSlices.State(independent));
        EXPECT_FLOAT_EQ(1, e.voices.Resolve(voice)->gain.target);
        EXPECT_FALSE(e.variableSlices.Reclaim(source, 100));
        EXPECT_FALSE(e.variableSlices.Reclaim(dependent, 100));
        if (cancelSource) {
            ASSERT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim,
                e.variableSlices.Invalidate(source, 1));
        }
        e.mixer.SetMaxCommandsPerQuantumForTest(1);
        e.nullBackend.Pump(1);
        EXPECT_GE(e.commands.SettledFrontier(), required);
        const auto state = cancelSource ? harken::VariableSliceState::Invalidated : harken::VariableSliceState::SliceApplied;
        EXPECT_EQ(state, e.variableSlices.State(source));
        EXPECT_EQ(state, e.variableSlices.State(dependent));
        EXPECT_EQ(10u, e.variableSlices.AppliedBlockSequence(source));
        EXPECT_EQ(10u, e.variableSlices.AppliedBlockSequence(dependent));
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f * (cancelSource ? 1 : DbToLinear(-6)),
                e.nullBackend.LastBlock()[2 * i], 1e-6f);
        EXPECT_EQ(cancelSource ? harken::kVariableSliceSourceRejected : 0u,
            e.variableSlices.InvalidationReasons(dependent));
    }
}

TEST(VariableCurrentSlice, InlineSourcesPublishWithoutNewCandidatesAndTerminalSweepWaitsForCalendar) {
    for (const bool failBeforeConsumption : {false, true}) {
        SCOPED_TRACE(failBeforeConsumption);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1));
        const auto frame = e.variables.EarliestReadCutFrame(e);
        harken::VariableReadCutRef cut{};
        ASSERT_TRUE(e.variables.CaptureReadCut(0, frame, e.logicCommits.Current() + 1, 0, &cut));
        float captured = 0;
        ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &captured));
        EXPECT_FLOAT_EQ(1, captured);
        harken::HeldCurrentSliceToken calendar;
        ASSERT_TRUE(e.variables.ReserveNativeReadCutCalendar(cut, frame, &calendar));
        const auto cutoff = e.timelinePool.PublishCutoffFrame();
        ASSERT_TRUE(e.variables.PrepareReadCutSources(e, cut));
        EXPECT_EQ(cutoff, e.timelinePool.PublishCutoffFrame());
        ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, frame));
        harken::VariableNativeStartIdentity identity{};
        constexpr VnVoiceHandle native = std::uint64_t{1} << 32;
        ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, native, frame, &identity));
        ASSERT_TRUE(e.variables.CommitNativeReadCut(identity));
        ASSERT_TRUE(e.timelinePool.ReleaseHeldCurrentSlice(&calendar));
        if (failBeforeConsumption) ASSERT_TRUE(e.variables.FinishNativeReadCut(identity, false));
        e.variables.AcknowledgeFinishedNativeReadCuts();
        EXPECT_TRUE(e.variables.HasNativePublicationHold(native));
        e.variables.PrepareClaimedSlices(e);
        EXPECT_GE(e.timelinePool.PublishCutoffFrame(), frame + kBlockFrames);
        while (e.mixer.BlocksRendered() * kBlockFrames <= frame) e.nullBackend.Pump(1);
        EXPECT_TRUE(e.variables.NativeReadCutCalendarConsumed(identity));
        if (!failBeforeConsumption) {
            ASSERT_TRUE(e.variables.MarkNativeReadCutInstalled(identity));
            ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.NativeReadCutSourcesStatus(identity));
            ASSERT_TRUE(e.variables.FinishNativeReadCut(identity, true));
        }
        e.variables.EndService(e);
        e.variables.AcknowledgeFinishedNativeReadCuts();
        EXPECT_FALSE(e.variables.HasNativePublicationHold(native));
        EXPECT_EQ(harken::VariableNativeStartReceipt::None, e.variables.NativeReadCutStatus(identity));
    }
}

TEST(VariableCurrentSlice, NativeVoicesPreserveOwnFirstOutputThenUseLatestVariableEndpoint) {
    for (const std::uint32_t workers : {0u, 2u}) {
        SCOPED_TRACE(workers);
        auto config = CurrentSliceConfig(3, kBlockFrames, kSampleRate);
        config.renderWorkerCount = workers;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const std::vector<float> samples(kSampleRate, 0.05f);
        const auto pcm = MonoDesc(samples, kSampleRate);
        const auto sound = harkentest::ObjectId(7103);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
        const auto view = e.registry.Lookup(sound);
        constexpr auto frame = 4 * kBlockFrames;
        std::array<VnVoiceHandle, 2> voices{};
        std::array<harken::VariableNativeStartIdentity, 2> identities{};
        for (std::size_t index = 0; index < voices.size(); ++index) {
            ASSERT_EQ(VN_OK, e.voices.Play(view, index == 0 ? -12 : -6, 1, 0, false, 0, 0, 0,
                harken::ResolvePanGains(e.config.mixChannelLayout, index == 0 ? -1 : 1),
                0, view.frames, 0, harken::SelectResampleProfile(kSampleRate, kSampleRate),
                &voices[index], e.buses.GenerationAt(0), UINT32_MAX, 0, true));
            harken::VariableReadCutRef cut{};
            ASSERT_TRUE(e.variables.CaptureReadCut(0, frame, index == 0 ? 2 : 4, 0, &cut));
            harken::HeldCurrentSliceToken held;
            ASSERT_TRUE(e.variables.ReserveNativeReadCutCalendar(cut, 0, &held));
            ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, frame));
            ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, voices[index], frame, &identities[index]));
            ASSERT_TRUE(e.voices.BindPreparedNativeReadCut(voices[index], identities[index]));
            ASSERT_TRUE(e.variables.CommitNativeReadCut(identities[index]));
            ASSERT_TRUE(e.voices.CommitPreparedVoice(voices[index]));
            ASSERT_TRUE(e.timelinePool.ReleaseHeldCurrentSlice(&held));
            harken::CommandRecord command{};
            command.opcode = harken::kOpcodePlay;
            command.payload.play.handle = voices[index];
            command.payload.play.sourceIdentity = voices[index];
            command.payload.play.batch = {harken::kPlayLedBatchVersion,
                harken::PlayLedBatchKind::DirectStart, 0, 1, 0};
            ASSERT_TRUE(e.commands.TryPush(command));
        }
        for (const std::uint64_t sequence : {1u, 3u, 5u}) {
            harken::VariableSliceRef slice{};
            ASSERT_TRUE(e.variableSlices.Reserve(e.variables.CurrentWorldGeneration(),
                e.variables.CurrentTransportEpoch(), sequence, frame, sequence, 2, &slice));
            for (const auto voice : voices) {
                harken::VariableSliceTarget target{};
                target.locator.scope = harken::VariableTargetScope::Voice;
                target.locator.object = voice;
                target.locator.objectGeneration = static_cast<std::uint32_t>(voice >> 32);
                target.locator.targetId = harken::kTargetVoiceVolume;
                target.targetRevision = 1;
                target.command.opcode = harken::kOpcodeSetVoiceVolume;
                target.command.payload.setVoiceVolume.handle = voice;
                target.command.payload.setVoiceVolume.volumeDb = sequence == 1 ? -12 : sequence == 3 ? -6 : -3;
                ASSERT_TRUE(e.variableSlices.AddTarget(slice, target));
            }
            ASSERT_TRUE(e.variableSlices.Publish(slice));
            harken::ScheduledCurrentSliceInput input{};
            input.executeFrame = frame;
            input.logicCommitSequence = sequence;
            input.slice = {slice.slot, slice.generation};
            harken::HeldCurrentSliceToken held;
            ASSERT_EQ(harken::ReserveStatus::Committed,
                e.timelinePool.ReserveHeldCurrentSlice(input, harken::PrePublishLatePolicy::Refuse, 0, &held).status);
            ASSERT_TRUE(e.timelinePool.ReleaseHeldCurrentSlice(&held));
        }
        e.timelinePool.Publish(frame + kBlockFrames);
        while (e.mixer.BlocksRendered() < 4) e.nullBackend.Pump(1);
        e.mixer.ArenaForTests().SetCapacityForTests(0);
        e.nullBackend.Pump(1);
        e.mixer.ArenaForTests().RestoreCapacityForTests();
        for (const auto& identity : identities)
            EXPECT_EQ(harken::VariableNativeStartReceipt::Installed, e.variables.NativeReadCutStatus(identity));
        e.nullBackend.Pump(1);
        const auto checkOutput = [&](float leftDb, float rightDb) {
            const float* output = e.nullBackend.LastBlock();
            for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i) {
                EXPECT_NEAR(0.05f * DbToLinear(leftDb) * std::sqrt(2.0f), output[i * 2], 1e-5f);
                EXPECT_NEAR(0.05f * DbToLinear(rightDb) * std::sqrt(2.0f), output[i * 2 + 1], 1e-5f);
            }
        };
        checkOutput(-12, -6);
        for (const auto& identity : identities) ASSERT_TRUE(e.variables.AcknowledgeNativeReadCut(identity));
        e.nullBackend.Pump(1);
        checkOutput(-3, -3);
    }
}

TEST(VariableCurrentSlice, FirstOutputOwnedWeightsKeepInitialMixAndAbsoluteSegmentTime) {
    for (const std::uint32_t workers : {0u, 2u}) {
        for (const bool timeline : {false, true}) {
            SCOPED_TRACE(::testing::Message() << workers << ':' << timeline);
            auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
            config.renderWorkerCount = workers;
            EngineSession session(config);
            ASSERT_EQ(VN_OK, session.InitResult());
            auto& e = *session.Engine();
            const std::vector<float> samples(kSampleRate, 0.05f);
            const auto pcm = MonoDesc(samples, kSampleRate);
            const auto sound = harkentest::ObjectId(7105);
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
            const auto view = e.registry.Lookup(sound);
            VnVoiceHandle voice = 0;
            ASSERT_EQ(VN_OK, e.voices.Play(view, 0, 1, 0, false, 0, 0, 0,
                harken::ResolvePanGains(e.config.mixChannelLayout, -1), 0, view.frames, 0,
                harken::SelectResampleProfile(kSampleRate, kSampleRate), &voice,
                e.buses.GenerationAt(0), UINT32_MAX, 0, true));
            ASSERT_TRUE(e.voices.BindPreparedFirstOutputOrder(voice, {0, 1, 0}));
            const harken::VariableWeightControlIdentity outer{101, 7, 0}, inner{102, 9, 1};
            harken::WeightSegmentV1 initial{0, 2, 0.5f, 0.5f, 1, {1, 1}, {1, 1}};
            const std::array initialWeights = {harken::VariableWeightControlBinding{outer, initial},
                harken::VariableWeightControlBinding{inner, initial}};
            ASSERT_TRUE(e.voices.PrepareVariableWeightControls(voice, initialWeights.data(), 2));
            ASSERT_TRUE(e.voices.CommitPreparedVoice(voice));
            e.voices.ApplyPlay(voice, voice, 0, 0);
            const harken::WeightSegmentV1 latest{0, 4 * kBlockFrames, 0.25f, 0.75f, 2, {2, 1}, {2, 1}};
            const harken::WeightSegmentV1 expired{0, 2, 0.2f, 0.6f, 2, {3, 1}, {3, 1}};
            struct Injection {
                harken::VoicePool* voices;
                VnVoiceHandle voice;
                std::array<harken::VariableWeightControlBinding, 2> weights;
                bool injected = false;
            } injection{&e.voices, voice, {{{outer, latest}, {inner, expired}}}};
            if (timeline) {
                e.mixer.SetCurrentSliceFrontierHookForTest(
                    [](void* context, CurrentSliceHookPhase phase, std::uint64_t block,
                        std::size_t, std::uint32_t, harken::TimelinePagePrefixStopReason) noexcept {
                        auto& input = *static_cast<Injection*>(context);
                        if (phase != CurrentSliceHookPhase::AfterExecuteBeforeApply || input.injected) return;
                        for (const auto& weight : input.weights) {
                            harken::VoiceTimelineEvent event{};
                            event.kind = harken::TimelineEventKind::VariableWeight;
                            event.offset = 10;
                            event.variableWeightOwner = weight.owner;
                            event.variableWeight = weight.segment;
                            event.order = {block * kBlockFrames + event.offset, 2, 0};
                            EXPECT_TRUE(input.voices->AppendTimelineEvent(static_cast<std::uint32_t>(input.voice),
                                static_cast<std::uint32_t>(input.voice >> 32), block, event));
                        }
                        input.injected = true;
                    }, &injection);
            } else {
                ASSERT_TRUE(e.voices.ApplyOwnedVariableWeightSegment(voice, outer, latest));
                ASSERT_TRUE(e.voices.ApplyOwnedVariableWeightSegment(voice, inner, expired));
                EXPECT_FALSE(e.voices.ApplyOwnedVariableWeightSegment(voice, outer, initial));
            }
            e.nullBackend.Pump(1);
            if (timeline) ASSERT_TRUE(injection.injected);
            for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
                EXPECT_NEAR(0.05f * 0.25f * std::sqrt(2.0f), e.nullBackend.LastBlock()[2 * i], 1e-6f);
            e.nullBackend.Pump(1);
            for (std::uint32_t i = 0; i < kBlockFrames; ++i) {
                const float weight = 0.25f + 0.5f * static_cast<float>(kBlockFrames + i) /
                    static_cast<float>(4 * kBlockFrames - 1);
                EXPECT_NEAR(0.05f * weight * 0.6f * std::sqrt(2.0f), e.nullBackend.LastBlock()[2 * i], 1e-6f);
            }
            const auto* render = e.voices.Resolve(voice);
            ASSERT_NE(nullptr, render);
            EXPECT_EQ(0u, render->variableWeightControls[0].segment.startFrame);
            EXPECT_EQ(4 * kBlockFrames, render->variableWeightControls[0].segment.endFrame);
        }
    }
}

TEST(VariableCurrentSlice, FirstOutputOwnedWeightRemovalDoesNotResurrectCancelledOwner) {
    EngineSession session(CurrentSliceConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    const std::vector<float> samples(kSampleRate, 0.05f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    const auto sound = harkentest::ObjectId(7106);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    const auto view = e.registry.Lookup(sound);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, e.voices.Play(view, 0, 1, 0, false, 0, 0, 0,
        harken::ResolvePanGains(e.config.mixChannelLayout, -1), 0, view.frames, 0,
        harken::SelectResampleProfile(kSampleRate, kSampleRate), &voice,
        e.buses.GenerationAt(0), UINT32_MAX, 0, true));
    ASSERT_TRUE(e.voices.BindPreparedFirstOutputOrder(voice, {0, 1, 0}));
    const harken::VariableWeightControlIdentity owner{101, 7, 0};
    const harken::VariableWeightControlBinding initial{owner, {0, 2, 0.5f, 0.5f, 1, {1, 1}, {1, 1}}};
    ASSERT_TRUE(e.voices.PrepareVariableWeightControls(voice, &initial, 1));
    ASSERT_TRUE(e.voices.CommitPreparedVoice(voice));
    e.voices.ApplyPlay(voice, voice, 0, 0);
    auto latest = initial.segment;
    latest.segmentGeneration = 2;
    latest.startWeight = latest.endWeight = 0.25f;
    ASSERT_TRUE(e.voices.ApplyOwnedVariableWeightSegment(voice, owner, latest));
    EXPECT_FALSE(e.voices.RemoveOwnedVariableWeightSegment(voice, owner, 1));
    ASSERT_TRUE(e.voices.RemoveOwnedVariableWeightSegment(voice, owner, 2));
    ++latest.segmentGeneration;
    EXPECT_FALSE(e.voices.ApplyOwnedVariableWeightSegment(voice, owner, latest));
    e.nullBackend.Pump(1);
    for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
        EXPECT_NEAR(0.025f * std::sqrt(2.0f), e.nullBackend.LastBlock()[2 * i], 1e-6f);
    e.nullBackend.Pump(1);
    for (std::uint32_t i = 0; i < kBlockFrames; ++i)
        EXPECT_NEAR(0.05f * std::sqrt(2.0f), e.nullBackend.LastBlock()[2 * i], 1e-6f);
    EXPECT_FALSE(e.voices.ApplyOwnedVariableWeightSegment(voice, owner, latest));
    ASSERT_NE(nullptr, e.voices.Resolve(voice));
    EXPECT_FALSE(e.voices.Resolve(voice)->variableWeightControls[0].active);
    EXPECT_EQ(2u, e.voices.Resolve(voice)->variableWeightControls[0].segment.segmentGeneration);
}

TEST(VariableCurrentSlice, FirstOutputSurvivesStreamStarvationUntilRealPcm) {
    for (const std::uint32_t workers : {0u, 2u}) {
        SCOPED_TRACE(workers);
        constexpr auto capacity = 8 * kBlockFrames;
        std::array<float, capacity> storage{};
        storage.fill(0.05f);
        float* planes[harken::kMaxChannels] = {storage.data()};
        harken::StreamRing ring;
        ring.Wire(planes, capacity);
        ring.ResetForUse(1);
        auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
        config.renderWorkerCount = workers;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, e.voices.PlayStream(&ring, 1, kSampleRate, kBlockFrames, -12, 1,
            0, false, 0, 0, 0, harken::ResolvePanGains(config.mixChannelLayout, -1),
            0, kSampleRate, 0, kSampleRate, 0,
            harken::SelectResampleProfile(kSampleRate, kSampleRate), &voice,
            e.buses.GenerationAt(0), UINT32_MAX, 0, nullptr, 0, true));
        ASSERT_TRUE(e.voices.BindPreparedFirstOutputOrder(voice, {0, 1, 0}));
        ASSERT_TRUE(e.voices.CommitPreparedVoice(voice));
        e.voices.ApplyPlay(voice, voice, 0, 0);
        harken::CommandRecord command{};
        command.opcode = harken::kOpcodeSetVoiceVolume;
        command.payload.setVoiceVolume.handle = voice;
        command.payload.setVoiceVolume.volumeDb = -6;
        ASSERT_TRUE(e.voices.DeferVariableCommandUntilFirstOutput(command, {0, 2, 0}, 1));
        e.nullBackend.Pump(2);
        ASSERT_NE(nullptr, e.voices.Resolve(voice));
        EXPECT_EQ(0u, e.voices.Resolve(voice)->cascade.produced);
        EXPECT_FLOAT_EQ(DbToLinear(-12), e.voices.Resolve(voice)->gain.target);
        for (std::uint32_t i = 0; i < 2 * kBlockFrames; ++i)
            EXPECT_FLOAT_EQ(0, e.nullBackend.LastBlock()[i]);
        ring.PublishWrite(capacity);
        e.nullBackend.Pump(1);
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f * DbToLinear(-12) * std::sqrt(2.0f), e.nullBackend.LastBlock()[2 * i], 1e-6f);
        e.nullBackend.Pump(1);
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f * DbToLinear(-6) * std::sqrt(2.0f), e.nullBackend.LastBlock()[2 * i], 1e-6f);
    }
}

TEST(VariableCurrentSlice, FirstOutputOldEffectAndStoppedGenerationCannotChangeReplacementPcm) {
    for (const bool stopAndReuse : {false, true}) {
        SCOPED_TRACE(stopAndReuse);
        auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
        config.fxSlotsPerVoice = 1;
        config.gainEffectPoolCapacity = 2;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        const std::vector<float> samples(kSampleRate, 0.05f);
        const auto pcm = MonoDesc(samples, kSampleRate);
        const auto sound = harkentest::ObjectId(7107);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
        const auto view = e.registry.Lookup(sound);
        const auto prepare = [&](VnVoiceHandle* voice) {
            return e.voices.Play(view, 0, 1, 0, false, 0, 0, 0,
                harken::ResolvePanGains(config.mixChannelLayout, -1), 0, view.frames, 0,
                harken::SelectResampleProfile(kSampleRate, kSampleRate), voice,
                e.buses.GenerationAt(0), UINT32_MAX, 0, true);
        };
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, prepare(&voice));
        ASSERT_TRUE(e.voices.BindPreparedFirstOutputOrder(voice, {0, 1, 0}));
        ASSERT_TRUE(e.voices.CommitPreparedVoice(voice));
        e.voices.ApplyPlay(voice, voice, 0, 0);
        const auto oldEffect = e.effectBank.Acquire(VN_EFFECT_GAIN);
        ASSERT_EQ(VN_EFFECT_GAIN, oldEffect.type);
        ASSERT_TRUE(e.voices.ApplyOwnedVoiceEffect(voice, 0, oldEffect));
        harken::CommandRecord command{};
        command.opcode = harken::kOpcodeSetVoiceEffectParam;
        command.payload.setVoiceEffectParam.handle = voice;
        command.payload.setVoiceEffectParam.slot = 0;
        command.payload.setVoiceEffectParam.effectType = oldEffect.type;
        command.payload.setVoiceEffectParam.value = -12;
        ASSERT_TRUE(e.voices.DeferVariableCommandUntilFirstOutput(command, {0, 2, 0}, 1,
            {oldEffect.type, oldEffect.poolIndex, oldEffect.generation}));
        if (stopAndReuse) {
            const auto stopped = voice;
            e.voices.ApplyStop(voice, 20, 0, e.notifications, 0);
            ASSERT_EQ(nullptr, e.voices.Resolve(stopped));
            ASSERT_EQ(VN_OK, prepare(&voice));
            EXPECT_EQ(static_cast<std::uint32_t>(stopped), static_cast<std::uint32_t>(voice));
            EXPECT_NE(stopped, voice);
            ASSERT_TRUE(e.voices.CommitPreparedVoice(voice));
            e.voices.ApplyPlay(voice, voice, 0, 0);
        }
        const auto replacement = e.effectBank.Acquire(VN_EFFECT_GAIN);
        ASSERT_EQ(VN_EFFECT_GAIN, replacement.type);
        ASSERT_TRUE(e.voices.ApplyOwnedVoiceEffect(voice, 0, replacement));
        e.nullBackend.Pump(2);
        const auto* state = static_cast<const harken::GainState*>(e.effectBank.LiveStateFor(replacement));
        ASSERT_NE(nullptr, state);
        EXPECT_FLOAT_EQ(1, state->targetGainLinear);
        for (std::uint32_t i = kBlockFrames / 2; i < kBlockFrames; ++i)
            EXPECT_NEAR(0.05f * std::sqrt(2.0f), e.nullBackend.LastBlock()[2 * i], 1e-6f);
    }
}

TEST(VariableCurrentSlice, FirstOutputToneUpdatesKeepEveryScalarAndTail) {
    for (const auto band : {0u, 1u, 2u}) {
        for (const bool reverse : {false, true}) {
            SCOPED_TRACE(::testing::Message() << band << ':' << reverse);
            auto config = CurrentSliceConfig(1, kBlockFrames, kSampleRate);
            config.fxSlotsPerVoice = 1;
            config.toneEffectPoolCapacity = 1;
            EngineSession session(config);
            ASSERT_EQ(VN_OK, session.InitResult());
            auto& e = *session.Engine();
            const std::vector<float> samples(kSampleRate, 0.05f);
            const auto pcm = MonoDesc(samples, kSampleRate);
            const auto sound = harkentest::ObjectId(7104);
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
            const auto view = e.registry.Lookup(sound);
            VnVoiceHandle voice = 0;
            ASSERT_EQ(VN_OK, e.voices.Play(view, 0, 1, 0, false, 0, 0, 0,
                harken::ResolvePanGains(e.config.mixChannelLayout, 0), 0, view.frames, 0,
                harken::SelectResampleProfile(kSampleRate, kSampleRate), &voice,
                e.buses.GenerationAt(0), UINT32_MAX, 0, true));
            ASSERT_TRUE(e.voices.BindPreparedFirstOutputOrder(voice, {0, 1, 0}));
            ASSERT_TRUE(e.voices.CommitPreparedVoice(voice));
            e.voices.ApplyPlay(voice, voice, 0, 0);
            const auto effect = e.effectBank.Acquire(VN_EFFECT_THREE_BAND_TONE);
            ASSERT_EQ(VN_EFFECT_THREE_BAND_TONE, effect.type);
            auto* state = static_cast<harken::ToneEffectState*>(e.effectBank.LiveStateFor(effect));
            ASSERT_NE(nullptr, state);
            ASSERT_TRUE(e.voices.ApplyOwnedVoiceEffect(voice, 0, effect));
            const auto gainParam = band == 0 ? harken::kToneParamLowShelfGainDb
                : band == 1 ? harken::kToneParamPeakGainDb : harken::kToneParamHighShelfGainDb;
            const auto freqParam = band == 0 ? harken::kToneParamLowShelfFreqHz
                : band == 1 ? harken::kToneParamPeakFreqHz : harken::kToneParamHighShelfFreqHz;
            std::array<float, harken::kToneParamCount> params{0, 200, 0, 1000, 1, 0, 6000};
            const float frequency = band == 0 ? 400.0f : band == 1 ? 2000.0f : 8000.0f;
            for (const auto param : reverse ? std::array{freqParam, gainParam} : std::array{gainParam, freqParam}) {
                params[param] = param == gainParam ? 6.0f : frequency;
                harken::BiquadCoeffs coeffs{};
                std::uint32_t tail = 0;
                ASSERT_TRUE(harken::PrepareToneSection(param, params.data(), kSampleRate, &coeffs, &tail));
                harken::CommandRecord command{};
                command.opcode = harken::kOpcodeSetVoiceEffectParam;
                command.payload.setVoiceEffectParam.handle = voice;
                command.payload.setVoiceEffectParam.slot = 0;
                command.payload.setVoiceEffectParam.paramIndex = param;
                command.payload.setVoiceEffectParam.value = params[param];
                command.payload.setVoiceEffectParam.effectType = effect.type;
                command.payload.setVoiceEffectParam.coeffs = harken::PrepareBiquadCoeffs(coeffs);
                command.payload.setVoiceEffectParam.tailFrames = tail;
                ASSERT_TRUE(e.voices.DeferVariableCommandUntilFirstOutput(command,
                    {0, param == (reverse ? freqParam : gainParam) ? 2u : 3u, 0}, 1,
                    {effect.type, effect.poolIndex, effect.generation},
                    {params[gainParam], params[freqParam], params[harken::kToneParamPeakQ], true}));
            }
            e.nullBackend.Pump(1);
            const float initialGain = band == 0 ? state->lowShelfGainDb
                : band == 1 ? state->peakGainDb : state->highShelfGainDb;
            EXPECT_FLOAT_EQ(0, initialGain);
            e.nullBackend.Pump(12);
            EXPECT_FLOAT_EQ(6, band == 0 ? state->lowShelfGainDb
                : band == 1 ? state->peakGainDb : state->highShelfGainDb);
            EXPECT_FLOAT_EQ(frequency, band == 0 ? state->lowShelfFreqHz
                : band == 1 ? state->peakFreqHz : state->highShelfFreqHz);
            const bool everActive = band == 0 ? state->lowShelfEverActive
                : band == 1 ? state->peakEverActive : state->highShelfEverActive;
            EXPECT_TRUE(everActive);
            const auto& transition = band == 0 ? state->lowShelfTransition
                : band == 1 ? state->peakTransition : state->highShelfTransition;
            EXPECT_FALSE(transition.transitioning);
            ASSERT_NE(nullptr, e.voices.Resolve(voice));
            EXPECT_GT(e.voices.Resolve(voice)->fxSlots[0].tailFrames(state), 0u);
        }
    }
}

TEST(VariableCurrentSlice, NativeReadCutCalendarRejectsDifferentHeldReservationWithIdenticalMetadata) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::VariableReadCutRef cut{};
    constexpr auto frame = 4 * kBlockFrames;
    ASSERT_TRUE(e.variables.CaptureReadCut(0, frame, 7, 3, &cut));
    harken::HeldCurrentSliceToken original, other;
    ASSERT_TRUE(e.variables.ReserveNativeReadCutCalendar(cut, 0, &original));
    harken::ScheduledCurrentSliceInput duplicate{};
    duplicate.executeFrame = frame;
    duplicate.logicCommitSequence = 7;
    duplicate.executionOrdinal = 3;
    duplicate.kind = harken::CurrentSliceKind::NativeStartCalendar;
    duplicate.slice = {cut.slot, cut.generation};
    ASSERT_EQ(harken::ReserveStatus::Committed,
        e.timelinePool.ReserveHeldCurrentSlice(duplicate, harken::PrePublishLatePolicy::Refuse, 0, &other).status);
    EXPECT_FALSE(e.variables.CancelNativeReadCutCalendar(cut, &other));
    EXPECT_TRUE(original.IsHeld());
    EXPECT_TRUE(other.IsHeld());
    EXPECT_FALSE(e.variables.ReleaseReadCut(cut));
    ASSERT_TRUE(e.timelinePool.CancelHeldCurrentSlice(&other));
    ASSERT_TRUE(e.variables.CancelNativeReadCutCalendar(cut, &original));
    EXPECT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, NativeReadCutCalendarRejectsDifferentConsumedReservationWithIdenticalMetadata) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::VariableReadCutRef cut{};
    constexpr auto frame = 4 * kBlockFrames;
    ASSERT_TRUE(e.variables.CaptureReadCut(0, frame, 7, 3, &cut));
    harken::HeldCurrentSliceToken original, other;
    ASSERT_TRUE(e.variables.ReserveNativeReadCutCalendar(cut, 0, &original));
    harken::ScheduledCurrentSliceInput duplicate{};
    duplicate.executeFrame = frame;
    duplicate.logicCommitSequence = 7;
    duplicate.executionOrdinal = 3;
    duplicate.kind = harken::CurrentSliceKind::NativeStartCalendar;
    duplicate.slice = {cut.slot, cut.generation};
    ASSERT_EQ(harken::ReserveStatus::Committed,
        e.timelinePool.ReserveHeldCurrentSlice(duplicate, harken::PrePublishLatePolicy::Refuse, 0, &other).status);
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, frame));
    harken::VariableNativeStartIdentity identity{};
    ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, std::uint64_t{1} << 32, frame, &identity));
    ASSERT_TRUE(e.variables.CommitNativeReadCut(identity));
    ASSERT_TRUE(e.variables.MarkNativeReadCutInstalled(identity));
    ASSERT_TRUE(e.timelinePool.ReleaseHeldCurrentSlice(&original));
    ASSERT_TRUE(e.timelinePool.ReleaseHeldCurrentSlice(&other));
    e.timelinePool.Publish(frame + kBlockFrames);
    std::array<harken::ConsumedCurrentSlice, 2> consumed{};
    ASSERT_EQ(2u, e.timelinePool.ConsumeCurrentSlicesRt(4, consumed.data(), consumed.size()));
    EXPECT_FALSE(e.variables.ConsumeNativeReadCutCalendar(consumed[1]));
    EXPECT_FALSE(e.variables.NativeReadCutCalendarConsumed(identity));
    EXPECT_FALSE(e.variables.FinishNativeReadCut(identity, true));
    ASSERT_TRUE(e.variables.ConsumeNativeReadCutCalendar(consumed[0]));
    ASSERT_TRUE(e.variables.FinishNativeReadCut(identity, true));
    EXPECT_TRUE(e.variables.AcknowledgeNativeReadCut(identity));
}

TEST(VariableCurrentSlice, NativeReadCutCalendarWithoutSourcesBlocksResetUntilDrainedAndAcknowledged) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::VariableReadCutRef cut{};
    constexpr auto frame = 4 * kBlockFrames;
    ASSERT_TRUE(e.variables.CaptureReadCut(0, frame, 1, 1, &cut));
    harken::HeldCurrentSliceToken held;
    ASSERT_TRUE(e.variables.ReserveNativeReadCutCalendar(cut, 0, &held));
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, frame));
    harken::VariableNativeStartIdentity identity{};
    ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, std::uint64_t{1} << 32, frame, &identity));
    ASSERT_TRUE(e.variables.CommitNativeReadCut(identity));
    ASSERT_TRUE(e.timelinePool.ReleaseHeldCurrentSlice(&held));
    ASSERT_TRUE(e.variables.FinishNativeReadCut(identity, false));
    EXPECT_FALSE(e.variables.ResetTransport(0));
    EXPECT_EQ(identity.transport, e.variables.CurrentTransportEpoch());
    e.timelinePool.Publish(frame + kBlockFrames);
    harken::ConsumedCurrentSlice consumed{};
    ASSERT_EQ(1u, e.timelinePool.ConsumeCurrentSlicesRt(4, &consumed, 1));
    ASSERT_TRUE(e.variables.ConsumeNativeReadCutCalendar(consumed));
    EXPECT_FALSE(e.variables.ResetTransport(0));
    ASSERT_TRUE(e.variables.AcknowledgeNativeReadCut(identity));
    EXPECT_TRUE(e.variables.ResetTransport(0));
    EXPECT_GT(e.variables.CurrentTransportEpoch(), identity.transport);
}

TEST(VariableCurrentSlice, NativeReadCutReceiptSurvivesOutcomeDeliveryAndReleasesPinsOnlyAfterAcknowledgment) {
    using Access = harken::detail::VariableServiceTestAccess;
    for (const bool outcomeFirst : {false, true}) {
        SCOPED_TRACE(outcomeFirst);
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const auto freeBefore = Access::OutcomeCapacity(e.variables)[2];
        const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
            e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
        const auto accepted = e.variables.AcceptGlobal(kVariable, 0.5f, observed, e.params, e.bindings, e.busTargets);
        ASSERT_EQ(VN_OK, accepted.result);
        harken::VariableReadCutRef cut{};
        ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, accepted.ticket.sequence + 1, 0, &cut));
        float value = 0;
        ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
        e.variables.BeginService(e, 0, kBlockFrames);
        e.variables.PrepareClaimedSlices(e);
        const auto source = e.variables.SliceForTicketForTests(accepted.ticket.sequence);
        ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
        harken::VariableNativeStartIdentity identity{};
        const VnVoiceHandle native = std::uint64_t{1} << 32;
        ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, native, 4 * kBlockFrames, &identity));
        EXPECT_FALSE(e.variables.ReleaseReadCut(cut));
        EXPECT_FALSE(e.variables.AcknowledgeNativeReadCut(identity));
        ASSERT_TRUE(e.variables.CommitNativeReadCut(identity));
        EXPECT_FALSE(e.variables.ReleaseReadCut(cut));
        EXPECT_FALSE(e.variables.CancelPreparedNativeReadCut(identity));
        EXPECT_FALSE(e.variables.FinishNativeReadCut(identity, true));
        EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.NativeReadCutSourcesStatus(identity));
        ASSERT_TRUE(e.variables.MarkNativeReadCutInstalled(identity));
        EXPECT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.NativeReadCutSourcesStatus(identity));
        EXPECT_FALSE(e.variables.FinishNativeReadCut(identity, true));
        for (std::uint32_t step = 0; step < 12 &&
            e.variables.NativeReadCutSourcesStatus(identity) == harken::VariableReadCutStatus::Waiting; ++step) e.nullBackend.Pump(1);
        ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.NativeReadCutSourcesStatus(identity));
        ASSERT_TRUE(e.variables.FinishNativeReadCut(identity, true));
        EXPECT_EQ(harken::VariableNativeStartReceipt::Activated, e.variables.NativeReadCutStatus(identity));
        EXPECT_FALSE(e.variables.FinishNativeReadCut(identity, false));
        EXPECT_FALSE(e.variables.ReleaseReadCut(cut));
        e.variables.EndService(e);
        if (!outcomeFirst) ASSERT_TRUE(e.variables.AcknowledgeNativeReadCut(identity));
        e.variables.MarkOutcomeDeliveryComplete();
        e.variables.FinishOutcomeDelivery();
        if (outcomeFirst) {
            e.nullBackend.Pump(1);
            e.variables.EndService(e);
            EXPECT_EQ(harken::VariableSliceState::SliceApplied, e.variableSlices.State(source));
            EXPECT_EQ(freeBefore - 1, Access::OutcomeCapacity(e.variables)[2]);
            ASSERT_TRUE(e.variables.AcknowledgeNativeReadCut(identity));
        }
        EXPECT_FALSE(e.variables.AcknowledgeNativeReadCut(identity));
        e.nullBackend.Pump(1);
        e.variables.EndService(e);
        EXPECT_EQ(freeBefore, Access::OutcomeCapacity(e.variables)[2]);
        EXPECT_EQ(harken::VariableSliceState::Free, e.variableSlices.State(source));
    }
}

TEST(VariableCurrentSlice, NativeReadCutReceiptRejectsEveryMismatchedIdentityAndStaleGeneration) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 1, 1, &cut));
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    const VnVoiceHandle native = std::uint64_t{1} << 32;
    harken::VariableNativeStartIdentity identity{};
    EXPECT_FALSE(e.variables.PrepareNativeReadCut(cut, native, 5 * kBlockFrames, &identity));
    EXPECT_FALSE(e.variables.PrepareNativeReadCut(cut, 0, 4 * kBlockFrames, &identity));
    ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, native, 4 * kBlockFrames, &identity));
    for (std::uint32_t changed = 0; changed < 5; ++changed) {
        SCOPED_TRACE(changed);
        auto wrong = identity;
        if (changed == 0) ++wrong.cut.generation;
        if (changed == 1) wrong.nativeHandle += std::uint64_t{1} << 32;
        if (changed == 2) ++wrong.world;
        if (changed == 3) ++wrong.transport;
        if (changed == 4) ++wrong.startFrame;
        EXPECT_EQ(harken::VariableNativeStartReceipt::None, e.variables.NativeReadCutStatus(wrong));
        EXPECT_FALSE(e.variables.CommitNativeReadCut(wrong));
        EXPECT_FALSE(e.variables.CancelPreparedNativeReadCut(wrong));
        EXPECT_FALSE(e.variables.MarkNativeReadCutInstalled(wrong));
        EXPECT_FALSE(e.variables.FinishNativeReadCut(wrong, false));
        EXPECT_FALSE(e.variables.AcknowledgeNativeReadCut(wrong));
    }
    ASSERT_TRUE(e.variables.CancelPreparedNativeReadCut(identity));
    ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
    harken::VariableReadCutRef next{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 1, 1, &next));
    EXPECT_EQ(cut.slot, next.slot);
    EXPECT_NE(cut.generation, next.generation);
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(next, 4 * kBlockFrames));
    harken::VariableNativeStartIdentity replacement{};
    ASSERT_TRUE(e.variables.PrepareNativeReadCut(next, native + (std::uint64_t{1} << 32),
        4 * kBlockFrames, &replacement));
    ASSERT_TRUE(e.variables.CommitNativeReadCut(replacement));
    EXPECT_FALSE(e.variables.FinishNativeReadCut(identity, false));
    EXPECT_EQ(harken::VariableNativeStartReceipt::Pending, e.variables.NativeReadCutStatus(replacement));
    ASSERT_TRUE(e.variables.FinishNativeReadCut(replacement, false));
    EXPECT_FALSE(e.variables.MarkNativeReadCutInstalled(replacement));
    ASSERT_TRUE(e.variables.AcknowledgeNativeReadCut(replacement));
}

TEST(VariableCurrentSlice, NativeReadCutFailedSourceCannotPublishActivatedReceipt) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    const auto accepted = e.variables.AcceptGlobal(kVariable, 1, observed, e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, accepted.result);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, accepted.ticket.sequence + 1, 0, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    e.variables.BeginService(e, 0, kBlockFrames);
    e.variables.PrepareClaimedSlices(e);
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    harken::VariableNativeStartIdentity identity{};
    ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, std::uint64_t{1} << 32, 4 * kBlockFrames, &identity));
    ASSERT_TRUE(e.variables.CommitNativeReadCut(identity));
    ASSERT_TRUE(e.variables.MarkNativeReadCutInstalled(identity));
    const auto source = e.variables.SliceForTicketForTests(accepted.ticket.sequence);
    ASSERT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim,
        e.variableSlices.Invalidate(source, 1));
    EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.NativeReadCutSourcesStatus(identity));
    EXPECT_FALSE(e.variables.FinishNativeReadCut(identity, true));
    ASSERT_TRUE(e.variables.FinishNativeReadCut(identity, false));
    EXPECT_EQ(harken::VariableNativeStartReceipt::Failed, e.variables.NativeReadCutStatus(identity));
    EXPECT_FALSE(e.variables.FinishNativeReadCut(identity, true));
    ASSERT_TRUE(e.variables.AcknowledgeNativeReadCut(identity));
}

TEST(VariableCurrentSlice, NativeReadCutShutdownAfterQuiescenceReleasesUnconfirmedHolders) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 1, 1, &cut));
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    harken::VariableNativeStartIdentity identity{};
    ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, std::uint64_t{1} << 32, 4 * kBlockFrames, &identity));
    ASSERT_TRUE(e.variables.CommitNativeReadCut(identity));
    e.variables.ShutdownReadCutsAfterQuiescence();
    EXPECT_EQ(harken::VariableNativeStartReceipt::None, e.variables.NativeReadCutStatus(identity));
    EXPECT_FALSE(e.variables.ReleaseReadCut(cut));
    harken::VariableReadCutRef next{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 1, 1, &next));
    EXPECT_EQ(cut.slot, next.slot);
    EXPECT_NE(cut.generation, next.generation);
    EXPECT_TRUE(e.variables.ReleaseReadCut(next));
}

TEST(VariableCurrentSlice, NativeReadCutShutdownReleasesRealSourcePinsWithoutFabricatingActivation) {
    using Access = harken::detail::VariableServiceTestAccess;
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto freeBefore = Access::OutcomeCapacity(e.variables)[2];
    const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
        e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
    const auto accepted = e.variables.AcceptGlobal(kVariable, 0.5f, observed, e.params, e.bindings, e.busTargets);
    ASSERT_EQ(VN_OK, accepted.result);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, accepted.ticket.sequence + 1, 0, &cut));
    float value = 0;
    ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
    e.variables.BeginService(e, 0, kBlockFrames);
    e.variables.PrepareClaimedSlices(e);
    const auto source = e.variables.SliceForTicketForTests(accepted.ticket.sequence);
    ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    harken::VariableNativeStartIdentity identity{};
    ASSERT_TRUE(e.variables.PrepareNativeReadCut(cut, std::uint64_t{1} << 32, 4 * kBlockFrames, &identity));
    ASSERT_TRUE(e.variables.CommitNativeReadCut(identity));
    ASSERT_TRUE(e.variables.MarkNativeReadCutInstalled(identity));
    for (std::uint32_t step = 0; step < 12 &&
        e.variables.NativeReadCutSourcesStatus(identity) == harken::VariableReadCutStatus::Waiting; ++step) e.nullBackend.Pump(1);
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.NativeReadCutSourcesStatus(identity));
    e.variables.EndService(e);
    e.variables.MarkOutcomeDeliveryComplete();
    e.variables.FinishOutcomeDelivery();
    e.nullBackend.Pump(1);
    e.variables.EndService(e);
    ASSERT_EQ(harken::VariableNativeStartReceipt::Installed, e.variables.NativeReadCutStatus(identity));
    ASSERT_EQ(freeBefore - 1, Access::OutcomeCapacity(e.variables)[2]);
    ASSERT_EQ(harken::VariableSliceState::SliceApplied, e.variableSlices.State(source));

    e.device.RevokeRenderAdmission();
    e.voices.DisableRenderWorkers();
    (void)e.device.StopCallback();
    (void)e.voices.UnbindRealtimeWorkerDomain();
    e.variables.ShutdownReadCutsAfterQuiescence();
    e.variables.ShutdownReadCutsAfterQuiescence();
    e.variables.EndService(e);
    EXPECT_EQ(harken::VariableNativeStartReceipt::None, e.variables.NativeReadCutStatus(identity));
    EXPECT_EQ(freeBefore, Access::OutcomeCapacity(e.variables)[2]);
    EXPECT_EQ(harken::VariableSliceState::Free, e.variableSlices.State(source));
    EXPECT_FALSE(e.variables.AcknowledgeNativeReadCut(identity));
}

TEST(VariableCurrentSlice, TimelineReadUsesShareCutUntilEveryLeafPassesItsOwnAudioFence) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 1, 1, &cut));
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    std::array<harken::VariableTimelineReadUseRef, 2> uses{};
    std::array<harken::TimelineArmToken, 2> tokens{};
    for (std::uint32_t index = 0; index < 2; ++index) {
        harken::TimelineArmTable::PreparedCommitInput input{};
        input.programGeneration = 7;
        input.pendingImmediateStart = true;
        ASSERT_TRUE(e.timelineArms.PreparePendingImmediate(index, 9, 7, {}, &input.arm));
        tokens[index] = input.arm;
        harken::TimelineArmTable::PreparedCommitProof proof;
        ASSERT_TRUE(e.timelineArms.ProvePreparedCommit(input, &proof));
        const auto native = (std::uint64_t{9} << 32) | index;
        ASSERT_TRUE(e.variables.PrepareTimelineReadUse(cut, native, e.timelineArms, proof, &uses[index]));
        EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.TimelineReadUseSourcesStatus(uses[index], native));
        harken::TimelineArmTable::ConsumePreparedCommit(std::move(proof));
        ASSERT_TRUE(e.variables.CommitTimelineReadUse(uses[index]));
        EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.TimelineReadUseSourcesStatus(uses[index], native));
        EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.TimelineReadUseSourcesStatus(uses[index], native + 1));
        EXPECT_TRUE(e.variables.HasNativePublicationHold(native));
        EXPECT_FALSE(e.variables.CancelPreparedTimelineReadUse(uses[index]));
        ASSERT_TRUE(e.timelineArms.AcknowledgeApplied(tokens[index], 2));
        ASSERT_TRUE(e.timelineArms.BindPendingImmediateStart(tokens[index], 4 * kBlockFrames, 4 * kBlockFrames));
        ASSERT_TRUE(e.timelineArms.Publish(tokens[index]));
    }
    ASSERT_TRUE(e.variables.TransferReadCutToTimelineUses(cut));
    EXPECT_FALSE(e.variables.TransferReadCutToTimelineUses(cut));
    EXPECT_FALSE(e.variables.ReleaseReadCut(cut));
    e.variables.AcknowledgeFinishedTimelineReadUses(100);
    EXPECT_EQ(1u, e.timelineArms.Resolve(tokens[0])->variableReadPins.load());
    ASSERT_TRUE(e.timelineArms.Start(tokens[0], 3));
    e.variables.AcknowledgeFinishedTimelineReadUses(3);
    EXPECT_TRUE(e.variables.HasNativePublicationHold(std::uint64_t{9} << 32));
    e.variables.AcknowledgeFinishedTimelineReadUses(4);
    EXPECT_FALSE(e.variables.HasNativePublicationHold(std::uint64_t{9} << 32));
    EXPECT_FALSE(e.variables.ReleaseReadCut(cut));
    EXPECT_EQ(harken::VariableReadCutStatus::Failed,
        e.variables.TimelineReadUseSourcesStatus(uses[0], std::uint64_t{9} << 32));
    ASSERT_TRUE(e.timelineArms.RequestCancel(tokens[1]));
    ASSERT_TRUE(e.timelineArms.Tombstone(tokens[1], 4));
    EXPECT_FALSE(e.timelineArms.Reclaim(tokens[1], 5));
    e.variables.AcknowledgeFinishedTimelineReadUses(4);
    EXPECT_FALSE(e.variables.ReleaseReadCut(cut));
    e.variables.AcknowledgeFinishedTimelineReadUses(5);
    EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.ReadCutSourcesStatus(cut));
    EXPECT_TRUE(e.timelineArms.Reclaim(tokens[1], 5));
    EXPECT_TRUE(e.timelineArms.Tombstone(tokens[0], 4));
    EXPECT_TRUE(e.timelineArms.Reclaim(tokens[0], 5));
    harken::VariableReadCutRef replacement{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 1, 1, &replacement));
    EXPECT_EQ(cut.slot, replacement.slot);
    EXPECT_NE(cut.generation, replacement.generation);
    EXPECT_TRUE(e.variables.ReleaseReadCut(replacement));
}

TEST(VariableCurrentSlice, TimelineReadUsePrivateRollbackDoesNotWaitForAnAudioReceipt) {
    for (const bool consumed : {false, true}) {
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        harken::VariableReadCutRef cut{};
        ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 1, 1, &cut));
        ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
        harken::TimelineArmTable::PreparedCommitInput input{};
        input.programGeneration = 7;
        input.pendingImmediateStart = true;
        ASSERT_TRUE(e.timelineArms.PreparePendingImmediate(0, 9, 7, {}, &input.arm));
        harken::TimelineArmTable::PreparedCommitProof proof;
        ASSERT_TRUE(e.timelineArms.ProvePreparedCommit(input, &proof));
        harken::VariableTimelineReadUseRef use{};
        ASSERT_TRUE(e.variables.PrepareTimelineReadUse(cut, std::uint64_t{9} << 32, e.timelineArms, proof, &use));
        if (consumed) harken::TimelineArmTable::ConsumePreparedCommit(std::move(proof));
        else proof.Release();
        EXPECT_FALSE(e.variables.CanReleaseReadCut(cut));
        EXPECT_FALSE(e.variables.ReleaseReadCut(cut));
        ASSERT_TRUE(e.variables.CancelPreparedTimelineReadUse(use));
        EXPECT_FALSE(e.variables.CancelPreparedTimelineReadUse(use));
        EXPECT_EQ(0u, e.timelineArms.Resolve(input.arm)->variableReadPins.load());
        EXPECT_EQ(0u, e.timelineArms.Resolve(input.arm)->armedCompletionSequence.load());
        EXPECT_TRUE(e.variables.CanReleaseReadCut(cut));
        ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
        if (!consumed) ASSERT_TRUE(e.timelineArms.CancelPrepared(input.arm));
        else {
            ASSERT_TRUE(e.timelineArms.RequestCancel(input.arm));
            ASSERT_TRUE(e.timelineArms.Tombstone(input.arm, 0));
            ASSERT_TRUE(e.timelineArms.Reclaim(input.arm, 1));
        }
    }
}

TEST(VariableCurrentSlice, TimelineReadUseCapacityRefusalReturnsPinsAndAllowsFreshGenerations) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    const auto storage = e.variables.ReadCutStorageBytes();
    EXPECT_EQ(e.voices.Capacity(), e.variables.TimelineReadUseCapacity());
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 1, 1, &cut));
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    harken::TimelineArmTable::PreparedCommitInput input{};
    input.programGeneration = 7;
    input.pendingImmediateStart = true;
    ASSERT_TRUE(e.timelineArms.PreparePendingImmediate(0, 9, 7, {}, &input.arm));
    harken::TimelineArmTable::PreparedCommitProof proof;
    ASSERT_TRUE(e.timelineArms.ProvePreparedCommit(input, &proof));
    std::vector<harken::VariableTimelineReadUseRef> uses(e.variables.TimelineReadUseCapacity());
    for (auto& use : uses)
        ASSERT_TRUE(e.variables.PrepareTimelineReadUse(cut, std::uint64_t{9} << 32, e.timelineArms, proof, &use));
    harken::VariableTimelineReadUseRef refused{};
    EXPECT_FALSE(e.variables.PrepareTimelineReadUse(cut, std::uint64_t{9} << 32, e.timelineArms, proof, &refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(uses.size(), e.timelineArms.RowAt(input.arm.slot)->variableReadPins.load());
    EXPECT_EQ(storage, e.variables.ReadCutStorageBytes());
    const auto old = uses.front();
    for (auto use : uses) ASSERT_TRUE(e.variables.CancelPreparedTimelineReadUse(use));
    ASSERT_TRUE(e.variables.PrepareTimelineReadUse(cut, std::uint64_t{9} << 32, e.timelineArms, proof, &refused));
    EXPECT_EQ(old.slot, refused.slot);
    EXPECT_NE(old.generation, refused.generation);
    EXPECT_FALSE(e.variables.CommitTimelineReadUse(old));
    ASSERT_TRUE(e.variables.CancelPreparedTimelineReadUse(refused));
    proof.Release();
    ASSERT_TRUE(e.timelineArms.CancelPrepared(input.arm));
    ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, TimelineReadUsesRetainRealSourceAcrossEveryLeafAndOutcomeDeliveryOrder) {
    using Access = harken::detail::VariableServiceTestAccess;
    for (const int variant : {0, 1, 2}) {
        EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        InstallVariable(&e);
        const auto freeBefore = Access::OutcomeCapacity(e.variables)[2];
        const auto observed = harken::ObserveVariableCallAt(e.variables.CurrentWorldGeneration(),
            e.variables.CurrentTransportEpoch(), kBlockFrames, 0, 0);
        const auto accepted = e.variables.AcceptGlobal(kVariable, 0.5f, observed, e.params, e.bindings, e.busTargets);
        ASSERT_EQ(VN_OK, accepted.result);
        harken::VariableReadCutRef cut{};
        ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, accepted.ticket.sequence + 1, 0, &cut));
        float value = 0;
        ASSERT_TRUE(e.variables.ReadCutValue(cut, kVariable, &value));
        EXPECT_FLOAT_EQ(0.5f, value);
        e.variables.BeginService(e, 0, kBlockFrames);
        e.variables.PrepareClaimedSlices(e);
        const auto source = e.variables.SliceForTicketForTests(accepted.ticket.sequence);
        ASSERT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
        std::array<harken::VariableTimelineReadUseRef, 2> uses{};
        std::array<harken::TimelineArmToken, 2> tokens{};
        for (std::uint32_t index = 0; index < 2; ++index) {
            harken::TimelineArmTable::PreparedCommitInput input{};
            input.programGeneration = 7;
            input.pendingImmediateStart = true;
            ASSERT_TRUE(e.timelineArms.PreparePendingImmediate(index, 9, 7, {}, &input.arm));
            tokens[index] = input.arm;
            harken::TimelineArmTable::PreparedCommitProof proof;
            ASSERT_TRUE(e.timelineArms.ProvePreparedCommit(input, &proof));
            const auto native = (std::uint64_t{9} << 32) | index;
            ASSERT_TRUE(e.variables.PrepareTimelineReadUse(cut, native, e.timelineArms, proof, &uses[index]));
            harken::TimelineArmTable::ConsumePreparedCommit(std::move(proof));
            ASSERT_TRUE(e.variables.CommitTimelineReadUse(uses[index]));
            EXPECT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.TimelineReadUseSourcesStatus(uses[index], native));
        }
        ASSERT_TRUE(e.variables.TransferReadCutToTimelineUses(cut));
        EXPECT_TRUE(e.variables.HasNativeReadCutsForTransport(e.variables.CurrentTransportEpoch()));
        for (std::uint32_t step = 0; step < 12 &&
            e.variables.ReadCutSourcesStatus(cut) == harken::VariableReadCutStatus::Waiting; ++step) e.nullBackend.Pump(1);
        ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.ReadCutSourcesStatus(cut));
        e.variables.EndService(e);
        if (variant != 1) {
            e.variables.MarkOutcomeDeliveryComplete();
            e.variables.FinishOutcomeDelivery();
        }
        e.nullBackend.Pump(1);
        e.variables.EndService(e);
        EXPECT_EQ(freeBefore - 1, Access::OutcomeCapacity(e.variables)[2]);
        EXPECT_EQ(harken::VariableSliceState::SliceApplied, e.variableSlices.State(source));
        if (variant == 2) {
            e.device.RevokeRenderAdmission();
            e.voices.DisableRenderWorkers();
            (void)e.device.StopCallback();
            (void)e.voices.UnbindRealtimeWorkerDomain();
            e.variables.ShutdownReadCutsAfterQuiescence();
            e.variables.ShutdownReadCutsAfterQuiescence();
        } else {
            const auto started = e.mixer.BlocksRendered();
            ASSERT_TRUE(e.timelineArms.AcknowledgeApplied(tokens[0], started));
            ASSERT_TRUE(e.timelineArms.BindPendingImmediateStart(tokens[0], 4 * kBlockFrames, 4 * kBlockFrames));
            ASSERT_TRUE(e.timelineArms.Publish(tokens[0]));
            ASSERT_TRUE(e.timelineArms.Start(tokens[0], started));
            e.variables.AcknowledgeFinishedTimelineReadUses(started);
            EXPECT_TRUE(e.variables.HasNativePublicationHold(std::uint64_t{9} << 32));
            e.nullBackend.Pump(1);
            e.variables.AcknowledgeFinishedTimelineReadUses(e.mixer.BlocksRendered());
            EXPECT_FALSE(e.variables.HasNativePublicationHold(std::uint64_t{9} << 32));
            e.variables.EndService(e);
            EXPECT_EQ(freeBefore - 1, Access::OutcomeCapacity(e.variables)[2]);
            EXPECT_EQ(harken::VariableSliceState::SliceApplied, e.variableSlices.State(source));
            const auto cancelled = e.mixer.BlocksRendered();
            ASSERT_TRUE(e.timelineArms.RequestCancel(tokens[1]));
            ASSERT_TRUE(e.timelineArms.Tombstone(tokens[1], cancelled));
            e.variables.AcknowledgeFinishedTimelineReadUses(cancelled);
            EXPECT_TRUE(e.variables.HasNativePublicationHold((std::uint64_t{9} << 32) | 1u));
            e.nullBackend.Pump(1);
            e.variables.AcknowledgeFinishedTimelineReadUses(e.mixer.BlocksRendered());
            if (variant == 1) {
                EXPECT_EQ(freeBefore - 1, Access::OutcomeCapacity(e.variables)[2]);
                e.variables.MarkOutcomeDeliveryComplete();
                e.variables.FinishOutcomeDelivery();
            }
        }
        e.variables.EndService(e);
        EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.ReadCutSourcesStatus(cut));
        EXPECT_EQ(freeBefore, Access::OutcomeCapacity(e.variables)[2]);
        EXPECT_EQ(harken::VariableSliceState::Free, e.variableSlices.State(source));
        EXPECT_FALSE(e.variables.HasNativeReadCutsForTransport(e.variables.CurrentTransportEpoch()));
        for (std::uint32_t index = 0; index < 2; ++index) {
            EXPECT_EQ(0u, e.timelineArms.RowAt(tokens[index].slot)->variableReadPins.load());
            EXPECT_FALSE(e.variables.CommitTimelineReadUse(uses[index]));
        }
    }
}

TEST(VariableCurrentSlice, TimelineReadUseRejectsForeignProofWrongNativeAndOnsetBeforeDecision) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, 4 * kBlockFrames, 1, 1, &cut));
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, 4 * kBlockFrames));
    harken::TimelineArmTable foreign(1);
    harken::TimelineArmTable::PreparedCommitInput input{};
    input.programGeneration = 7;
    input.startFrame = 3 * kBlockFrames;
    input.armDeadline = input.startFrame;
    ASSERT_TRUE(e.timelineArms.Prepare(0, 9, input.startFrame, input.armDeadline, 7, {}, &input.arm));
    harken::TimelineArmTable::PreparedCommitProof proof;
    ASSERT_TRUE(e.timelineArms.ProvePreparedCommit(input, &proof));
    harken::VariableTimelineReadUseRef use{};
    EXPECT_FALSE(e.variables.PrepareTimelineReadUse(cut, std::uint64_t{9} << 32, foreign, proof, &use));
    EXPECT_FALSE(e.variables.PrepareTimelineReadUse(cut, std::uint64_t{8} << 32, e.timelineArms, proof, &use));
    EXPECT_FALSE(e.variables.PrepareTimelineReadUse(cut, std::uint64_t{9} << 32, e.timelineArms, proof, &use));
    EXPECT_FALSE(use.Valid());
    EXPECT_EQ(0u, e.timelineArms.RowAt(input.arm.slot)->variableReadPins.load());
    proof.Release();
    ASSERT_TRUE(e.timelineArms.CancelPrepared(input.arm));
    ASSERT_TRUE(e.variables.ReleaseReadCut(cut));
}

TEST(VariableCurrentSlice, TimelineReadUseStartAuthorityChecksFinalBoundFrameAfterInstallationAck) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    constexpr std::uint64_t capturedFrame = 4 * kBlockFrames;
    for (const bool matchingFrame : {true, false}) {
        harken::VariableReadCutRef cut{};
        ASSERT_TRUE(e.variables.CaptureReadCut(0, capturedFrame, 1, 1, &cut));
        ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, capturedFrame));
        harken::TimelineArmTable::PreparedCommitInput input{};
        input.programGeneration = 7;
        input.pendingImmediateStart = true;
        ASSERT_TRUE(e.timelineArms.PreparePendingImmediate(0, 9, 7, {}, &input.arm));
        harken::TimelineArmTable::PreparedCommitProof proof;
        ASSERT_TRUE(e.timelineArms.ProvePreparedCommit(input, &proof));
        harken::VariableTimelineReadUseRef use{};
        constexpr auto native = std::uint64_t{9} << 32;
        ASSERT_TRUE(e.variables.PrepareTimelineReadUse(cut, native, e.timelineArms, proof, &use));
        harken::TimelineArmTable::ConsumePreparedCommit(std::move(proof));
        ASSERT_TRUE(e.variables.CommitTimelineReadUse(use));
        ASSERT_TRUE(e.variables.TransferReadCutToTimelineUses(cut));
        EXPECT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.TimelineReadUseStartStatus(use, native));
        EXPECT_TRUE(e.variables.PendingTimelineReadUseMatchesArm(use, native, input.arm));
        for (unsigned mismatch = 0; mismatch < 4; ++mismatch) {
            auto foreign = input.arm;
            if (mismatch == 0) ++foreign.slot;
            if (mismatch == 1) ++foreign.generation;
            if (mismatch == 2) ++foreign.renderSlot;
            if (mismatch == 3) ++foreign.renderGeneration;
            EXPECT_FALSE(e.variables.PendingTimelineReadUseMatchesArm(use, native, foreign));
        }
        EXPECT_FALSE(e.variables.PendingTimelineReadUseMatchesArm(use, native + 1, input.arm));
        ASSERT_TRUE(e.timelineArms.AcknowledgeApplied(input.arm, 2));
        EXPECT_EQ(harken::VariableReadCutStatus::Waiting, e.variables.TimelineReadUseStartStatus(use, native));
        const auto finalFrame = matchingFrame ? capturedFrame : capturedFrame + kBlockFrames;
        ASSERT_TRUE(e.timelineArms.BindPendingImmediateStart(input.arm, finalFrame, finalFrame));
        ASSERT_TRUE(e.timelineArms.Publish(input.arm));
        EXPECT_FALSE(e.variables.PendingTimelineReadUseMatchesArm(use, native, input.arm));
        EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.TimelineReadUseSourcesStatus(use, native));
        EXPECT_EQ(matchingFrame ? harken::VariableReadCutStatus::Applied : harken::VariableReadCutStatus::Failed,
            e.variables.TimelineReadUseStartStatus(use, native));
        EXPECT_EQ(harken::VariableReadCutStatus::Failed, e.variables.TimelineReadUseStartStatus(use, native + 1));
        ASSERT_TRUE(e.timelineArms.RequestCancel(input.arm));
        ASSERT_TRUE(e.timelineArms.Tombstone(input.arm, 3));
        e.variables.AcknowledgeFinishedTimelineReadUses(4);
        ASSERT_TRUE(e.timelineArms.Reclaim(input.arm, 4));
    }
}

TEST(VariableCurrentSlice, SharedTimelineReadCutAcceptsEachLeafsActualLaterOnset) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    constexpr auto decisionFrame = 4 * kBlockFrames;
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, decisionFrame, 1, 1, &cut));
    std::array<harken::VariableTimelineReadUseRef, 2> uses{};
    std::array<harken::TimelineArmToken, 2> arms{};
    for (std::uint32_t leaf = 0; leaf < uses.size(); ++leaf) {
        const auto onset = decisionFrame + leaf * kBlockFrames;
        harken::TimelineArmTable::PreparedCommitInput input{};
        input.programGeneration = 7;
        input.startFrame = input.armDeadline = onset;
        ASSERT_TRUE(e.timelineArms.Prepare(leaf, 9, onset, onset, 7, {}, &input.arm));
        arms[leaf] = input.arm;
        harken::TimelineArmTable::PreparedCommitProof proof;
        ASSERT_TRUE(e.timelineArms.ProvePreparedCommit(input, &proof));
        const auto native = (std::uint64_t{9} << 32) | leaf;
        ASSERT_TRUE(e.variables.PrepareTimelineReadUse(cut, native, e.timelineArms, proof, &uses[leaf]));
        harken::TimelineArmTable::ConsumePreparedCommit(std::move(proof));
        ASSERT_TRUE(e.variables.CommitTimelineReadUse(uses[leaf]));
        ASSERT_TRUE(e.timelineArms.AcknowledgeApplied(arms[leaf], 2));
        ASSERT_TRUE(e.timelineArms.Publish(arms[leaf]));
    }
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, decisionFrame));
    ASSERT_TRUE(e.variables.TransferReadCutToTimelineUses(cut));
    for (std::uint32_t leaf = 0; leaf < uses.size(); ++leaf) {
        const auto native = (std::uint64_t{9} << 32) | leaf;
        EXPECT_EQ(harken::VariableReadCutStatus::Applied, e.variables.TimelineReadUseStartStatus(uses[leaf], native));
        ASSERT_TRUE(e.timelineArms.RequestCancel(arms[leaf]));
        ASSERT_TRUE(e.timelineArms.Tombstone(arms[leaf], 3));
    }
    e.variables.AcknowledgeFinishedTimelineReadUses(4);
    EXPECT_FALSE(e.variables.CanReleaseReadCut(cut));
    for (const auto arm : arms) ASSERT_TRUE(e.timelineArms.Reclaim(arm, 4));
}

TEST(VariableCurrentSlice, OperationInitializerRefusalPreservesStartAndAllowsExactRetry) {
    EngineSession session(CurrentSliceConfig(2, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    InstallVariable(&e);
    constexpr auto onset = 4 * kBlockFrames + 1;
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(e.variables.CaptureReadCut(0, onset, 1, 1, &cut));
    harken::TimelineArmTable::PreparedCommitInput input{};
    input.programGeneration = 7;
    input.startFrame = input.armDeadline = onset;
    ASSERT_TRUE(e.timelineArms.Prepare(0, 9, onset, onset, 7, {}, &input.arm));
    harken::TimelineArmTable::PreparedCommitProof preparation;
    ASSERT_TRUE(e.timelineArms.ProvePreparedCommit(input, &preparation));
    constexpr auto native = std::uint64_t{9} << 32;
    harken::VariableTimelineReadUseRef use{};
    ASSERT_TRUE(e.variables.PrepareTimelineReadUse(cut, native, e.timelineArms, preparation, &use));
    harken::TimelineArmTable::ConsumePreparedCommit(std::move(preparation));
    ASSERT_TRUE(e.variables.CommitTimelineReadUse(use));
    ASSERT_EQ(harken::VariableReadCutStatus::Applied, e.variables.PublishReadCut(cut, onset));
    ASSERT_TRUE(e.variables.TransferReadCutToTimelineUses(cut));
    harken::TimelineArmReaderPin pin;
    ASSERT_TRUE(e.timelineArms.PinFrontierReader(input.arm, &pin));
    ASSERT_TRUE(e.timelineArms.AcknowledgeApplied(input.arm, 2));
    ASSERT_TRUE(e.timelineArms.Publish(input.arm));
    harken::TimelineProgramView program{};
    program.generation = input.programGeneration;
    harken::FrontierDueOperationRef operation{};
    operation.kind = harken::FrontierOperationKind::Dependent;
    operation.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    operation.order = {onset, 1};
    operation.transport = operation.world = 1;
    operation.transactionId = 1;
    operation.transactionSlot = 0;
    operation.program = &program;
    operation.programGeneration = program.generation;
    operation.armSlot = input.arm.slot;
    operation.armGeneration = input.arm.generation;
    operation.renderSlot = input.arm.renderSlot;
    operation.renderGeneration = input.arm.renderGeneration;
    harken::TimelineArmTable::AudioOperationClaim claim;
    ASSERT_EQ(harken::TimelineArmOperationDisposition::Claimed,
        e.timelineArms.BeginFrontierOperation(input.arm, operation, harken::TimelineArmOperationKind::Start, 5, &claim));
    harken::VariableTimelineInitializerProof initializer;
    EXPECT_FALSE(e.variables.ProveTimelineReadUseInitializer(use, native + 1, claim, &initializer));
    EXPECT_FALSE(initializer.Valid());
    e.timelineArms.CancelFrontierOperation(claim);
    auto* row = e.timelineArms.Resolve(input.arm);
    ASSERT_NE(nullptr, row);
    EXPECT_EQ(harken::ArmState::Published, row->state.load());
    EXPECT_FALSE(row->frontierOperation.Valid());
    EXPECT_EQ(0u, row->startedBlockSequence.load());
    EXPECT_EQ(5u, row->frontierLastReadSequence.load());
    ASSERT_EQ(harken::TimelineArmOperationDisposition::Claimed,
        e.timelineArms.BeginFrontierOperation(input.arm, operation, harken::TimelineArmOperationKind::Start, 5, &claim));
    ASSERT_TRUE(e.variables.ProveTimelineReadUseInitializer(use, native, claim, &initializer));
    ASSERT_TRUE(initializer.Valid());
    e.variables.ApplyTimelineReadUseInitializer(initializer, nullptr,
        [](void*, const harken::CommandRecord&) noexcept {});
    e.timelineArms.ApplyFrontierOperation(claim);
    e.timelineArms.FinishFrontierOperation(claim);
    EXPECT_EQ(harken::ArmState::Started, row->state.load());
    EXPECT_EQ(5u, row->startedBlockSequence.load());
    e.variables.AcknowledgeFinishedTimelineReadUses(6);
    EXPECT_EQ(0u, row->variableReadPins.load());
    ASSERT_TRUE(e.timelineArms.RequestCancelStartedVoice(input.arm));
    ASSERT_TRUE(e.timelineArms.Tombstone(input.arm, 5));
    ASSERT_TRUE(pin.Release(6));
    ASSERT_TRUE(e.timelineArms.Reclaim(input.arm, 6));
}

} // namespace
