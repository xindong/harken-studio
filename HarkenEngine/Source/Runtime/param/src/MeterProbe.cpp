#include "harken/param/MeterProbe.h"

#include "Engine.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/param/ParamTargetTable.h"
#include "harken/dsp/GainRamp.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <new>
#include <cmath>
#include <exception>
#include <tuple>

namespace harken {

namespace {

bool RemovalContains(const std::vector<BusSlotRef>& refs,
                     std::uint32_t index, std::uint32_t generation) noexcept {
    for (const auto ref : refs)
        if (ref.index == index && ref.generation == generation) return true;
    return false;
}

bool RemovalTargetsAttachment(const std::vector<BusSlotRef>& refs,
                              const BindingTable::Attachment& row) noexcept {
    return (row.scopeKind == VN_TARGET_SCOPE_BUS || row.scopeKind == VN_TARGET_SCOPE_BUS_EFFECT_PARAM) &&
        RemovalContains(refs, row.busIndex, row.targetGeneration);
}

bool SameRemovalAttachment(const BindingTable::Attachment& a,
                           const BindingTable::Attachment& b) noexcept {
    return a.used == b.used && a.bindingId == b.bindingId && a.definitionIndex == b.definitionIndex &&
        a.scopeKind == b.scopeKind && a.objectId == b.objectId && a.busIndex == b.busIndex &&
        a.targetGeneration == b.targetGeneration &&
        std::bit_cast<std::uint32_t>(a.lastContribution) == std::bit_cast<std::uint32_t>(b.lastContribution) &&
        a.lastHadValue == b.lastHadValue && a.meterFrozen == b.meterFrozen && a.ownerCookie == b.ownerCookie;
}

void DetachProbeAttachment(Engine& engine,
                           BindingTable::Attachment& attachment,
                           const BindingTable::Definition& def) {
    if (attachment.lastHadValue) {
        if (attachment.scopeKind == VN_TARGET_SCOPE_BUS) {
            engine.busTargets.NoteMeterProbeRemoved(
                attachment.busIndex, attachment.targetGeneration,
                def.targetId, def.transitionMs);
        } else if (attachment.scopeKind == VN_TARGET_SCOPE_VOICE) {
            const VnVoiceHandle render =
                engine.ledger.ToRender(attachment.instanceHandle);
            if (render != 0 &&
                static_cast<std::uint32_t>(render >> 32) ==
                    attachment.targetGeneration) {
                engine.params.ClearMeterProbeContribution(
                    render, def.id, def.targetId, def.transitionMs);
            }
        }
    }
    engine.bindings.ReleaseProbeAttachment(attachment);
}

bool SameLocator(const VariableTargetLocator& a,
                 const VariableTargetLocator& b) noexcept {
    return a.scope == b.scope && a.object == b.object &&
           a.objectGeneration == b.objectGeneration &&
           a.targetId == b.targetId && a.effectSlot == b.effectSlot &&
           a.paramIndex == b.paramIndex && a.effectType == b.effectType;
}

bool LocatorLess(const VariableTargetLocator& a,
                 const VariableTargetLocator& b) noexcept {
    return std::tie(a.scope, a.object, a.objectGeneration, a.targetId,
                    a.effectSlot, a.paramIndex, a.effectType) <
           std::tie(b.scope, b.object, b.objectGeneration, b.targetId,
                    b.effectSlot, b.paramIndex, b.effectType);
}

std::uint64_t AddFence(std::uint64_t appliedBlock,
                       std::uint32_t sampleRate,
                       std::uint32_t blockFrames,
                       std::uint32_t rampMs) noexcept {
    const std::uint64_t rampFrames =
        (static_cast<std::uint64_t>(sampleRate) * rampMs + 999u) / 1000u;
    const std::uint64_t rampBlocks =
        blockFrames == 0 ? 0
                         : (rampFrames + blockFrames - 1u) / blockFrames;
    return appliedBlock > UINT64_MAX - rampBlocks - 1u
               ? UINT64_MAX
               : appliedBlock + rampBlocks + 1u;
}

} // namespace

MeterProbeSystem::MeterProbeSystem(
    std::uint32_t maxBindingAttachments,
    std::uint32_t maxTargetsPerCurrentSlice)
    : withdrawalScratch_(maxBindingAttachments),
      maxTargetsPerCurrentSlice_(maxTargetsPerCurrentSlice) {}

bool MeterProbeSystem::ResolveWithdrawalTarget(
    const Engine& engine, const BindingTable::Attachment& attachment,
    const BindingTable::Definition& definition,
    WithdrawalTarget* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    WithdrawalTarget target{};
    if (attachment.scopeKind == VN_TARGET_SCOPE_BUS) {
        if (engine.busTargets.ActiveGeneration(attachment.busIndex) !=
            attachment.targetGeneration) {
            return false;
        }
        target.locator.scope = VariableTargetScope::Bus;
        target.locator.object = attachment.busIndex;
        target.locator.objectGeneration = attachment.targetGeneration;
        target.locator.targetId = definition.targetId;
        target.revision = engine.busTargets.TargetRevision(
            attachment.busIndex, attachment.targetGeneration,
            definition.targetId);
    } else if (attachment.scopeKind == VN_TARGET_SCOPE_VOICE) {
        const VnVoiceHandle render =
            engine.ledger.ToRender(attachment.instanceHandle);
        if (render == 0 ||
            static_cast<std::uint32_t>(render >> 32) !=
                attachment.targetGeneration) {
            return false;
        }
        target.locator.scope = VariableTargetScope::Voice;
        target.locator.object = render;
        target.locator.objectGeneration = attachment.targetGeneration;
        target.locator.targetId = definition.targetId;
        target.revision =
            engine.params.TargetRevision(render, definition.targetId);
    } else {
        const EffectParamLocator effect =
            UnpackEffectParamLocator(definition.targetId);
        const bool voice =
            attachment.scopeKind == VN_TARGET_SCOPE_VOICE_EFFECT_PARAM;
        const std::uint64_t object =
            voice ? engine.ledger.ToRender(attachment.instanceHandle)
                  : attachment.busIndex;
        if ((voice &&
             (object == 0 ||
              static_cast<std::uint32_t>(object >> 32) !=
                  attachment.targetGeneration)) ||
            (!voice &&
             engine.busTargets.ActiveGeneration(attachment.busIndex) !=
                 attachment.targetGeneration)) {
            return false;
        }
        target.locator.scope = voice ? VariableTargetScope::VoiceEffect
                                     : VariableTargetScope::BusEffect;
        target.locator.object = object;
        target.locator.objectGeneration = attachment.targetGeneration;
        target.locator.effectSlot = effect.slot;
        target.locator.paramIndex = effect.paramIndex;
        target.locator.effectType = effect.effectType;
        const EffectParamTargetStore::Scope scope =
            voice ? EffectParamTargetStore::Scope::Voice
                  : EffectParamTargetStore::Scope::Bus;
        if (!engine.effectParamTargets.ExactTargetRevision(
                scope, object, attachment.targetGeneration, effect.slot,
                effect.paramIndex, effect.effectType, &target.revision)) {
            return false;
        }
    }
    if (target.revision == 0) {
        return false;
    }
    *out = target;
    return true;
}

bool MeterProbeSystem::PrepareWithdrawalChunk(
    Engine& engine, MeterSystem::Probe& probe) {
    if (probe.withdrawalSlice.Valid() ||
        probe.withdrawalAllParticipantsReleased) {
        return true;
    }
    if (probe.withdrawalLogicSequence == 0) {
        return false;
    }
    std::uint32_t count = 0;
    engine.bindings.ForEachProbeAttachment(
        [&](const BindingTable::Attachment& attachment,
            const BindingTable::Definition& definition) {
            if (definition.probeStableId !=
                    probe.identity.probeStableId ||
                definition.probeGeneration !=
                    probe.identity.probeGeneration ||
                !attachment.lastHadValue) {
                return;
            }
            WithdrawalTarget target{};
            if (!ResolveWithdrawalTarget(engine, attachment, definition,
                                         &target)) {
                return;
            }
            target.rampMs = definition.transitionMs;
            if (count >= withdrawalScratch_.size()) {
                std::terminate();
            }
            withdrawalScratch_[count++] = target;
        });
    std::sort(withdrawalScratch_.begin(),
              withdrawalScratch_.begin() + count,
              [](const WithdrawalTarget& a,
                 const WithdrawalTarget& b) {
                  return LocatorLess(a.locator, b.locator);
              });
    std::uint32_t uniqueCount = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (uniqueCount != 0 &&
            SameLocator(withdrawalScratch_[uniqueCount - 1].locator,
                        withdrawalScratch_[i].locator)) {
            withdrawalScratch_[uniqueCount - 1].rampMs = std::max(
                withdrawalScratch_[uniqueCount - 1].rampMs,
                withdrawalScratch_[i].rampMs);
            continue;
        }
        withdrawalScratch_[uniqueCount++] = withdrawalScratch_[i];
    }
    if (uniqueCount == 0) {
        ReleaseWithdrawalChunkRows(engine, probe);
        probe.withdrawalAllParticipantsReleased = true;
        probe.withdrawalAudioFenceBlock = std::max(
            probe.withdrawalAudioFenceBlock,
            engine.mixer.BlocksRendered() == UINT64_MAX
                ? UINT64_MAX
                : engine.mixer.BlocksRendered() + 1u);
        probe.withdrawalSettled = true;
        return true;
    }
    const std::uint32_t chunkCount =
        std::min({uniqueCount, maxTargetsPerCurrentSlice_,
                  engine.variableSlices.TargetPoolCapacity()});
    if (chunkCount == 0) {
        return false;
    }
    VariableSliceRef slice{};
    const std::uint64_t blocks = engine.mixer.BlocksRendered();
    const std::uint64_t frame =
        engine.config.blockFrames == 0 ||
                blocks > UINT64_MAX / engine.config.blockFrames
            ? UINT64_MAX
            : blocks * engine.config.blockFrames;
    if (!engine.variableSlices.Reserve(
            engine.variables.CurrentWorldGeneration(),
            engine.variables.CurrentTransportEpoch(),
            0, frame,
            probe.identity.probeGeneration, chunkCount, &slice)) {
        return false;
    }
    for (std::uint32_t i = 0; i < chunkCount; ++i) {
        VariableSliceTarget target{};
        target.locator = withdrawalScratch_[i].locator;
        target.targetRevision = withdrawalScratch_[i].revision;
        target.rampMs = withdrawalScratch_[i].rampMs;
        if (!engine.variableSlices.AddTarget(slice, target)) {
            (void)engine.variableSlices.AbandonPrepared(slice);
            return false;
        }
    }
    std::uint32_t claimed = 0;
    for (; claimed < chunkCount; ++claimed) {
        const WithdrawalTarget& target = withdrawalScratch_[claimed];
        bool ok = false;
        switch (target.locator.scope) {
        case VariableTargetScope::Voice:
            ok = engine.params.TryClaim(
                static_cast<VnVoiceHandle>(target.locator.object),
                target.locator.targetId, slice, target.revision);
            break;
        case VariableTargetScope::Bus:
            ok = engine.busTargets.TryClaim(
                static_cast<std::uint32_t>(target.locator.object),
                target.locator.objectGeneration, target.locator.targetId,
                slice, target.revision);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            ok = engine.effectParamTargets.TryClaim(
                target.locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target.locator.object, target.locator.objectGeneration,
                target.locator.effectSlot, target.locator.paramIndex, slice,
                target.revision);
            break;
        }
        if (!ok) {
            break;
        }
    }
    if (claimed != chunkCount) {
        for (std::uint32_t i = claimed; i > 0; --i) {
            const WithdrawalTarget& target = withdrawalScratch_[i - 1];
            switch (target.locator.scope) {
            case VariableTargetScope::Voice:
                engine.params.ReleaseClaim(
                    static_cast<VnVoiceHandle>(target.locator.object),
                    target.locator.targetId, slice);
                break;
            case VariableTargetScope::Bus:
                engine.busTargets.ReleaseClaim(
                    static_cast<std::uint32_t>(target.locator.object),
                    target.locator.objectGeneration,
                    target.locator.targetId, slice);
                break;
            case VariableTargetScope::VoiceEffect:
            case VariableTargetScope::BusEffect:
                engine.effectParamTargets.ReleaseClaim(
                    target.locator.scope ==
                            VariableTargetScope::VoiceEffect
                        ? EffectParamTargetStore::Scope::Voice
                        : EffectParamTargetStore::Scope::Bus,
                    target.locator.object,
                    target.locator.objectGeneration,
                    target.locator.effectSlot,
                    target.locator.paramIndex, slice);
                break;
            }
        }
        (void)engine.variableSlices.AbandonPrepared(slice);
        return false;
    }
    probe.withdrawalSlice = slice;
    probe.withdrawalChunkCommandFrontier =
        engine.commands.PublishedHead();
    if (!probe.withdrawalFrontierCaptured) {
        probe.withdrawalCommandFrontier =
            probe.withdrawalChunkCommandFrontier;
        probe.withdrawalFrontierCaptured = true;
    }
    probe.withdrawalProjected = false;
    probe.withdrawalSliceCaptured = false;
    probe.withdrawalPageCommitted = false;
    return true;
}

bool MeterProbeSystem::AttachmentInWithdrawalChunk(
    const Engine& engine, const MeterSystem::Probe& probe,
    const BindingTable::Attachment& attachment,
    const BindingTable::Definition& definition) const noexcept {
    if (!probe.withdrawalSlice.Valid()) {
        return false;
    }
    WithdrawalTarget resolved{};
    if (!ResolveWithdrawalTarget(engine, attachment, definition,
                                 &resolved)) {
        return false;
    }
    const std::uint32_t count =
        engine.variableSlices.TargetCount(probe.withdrawalSlice);
    for (std::uint32_t i = 0; i < count; ++i) {
        const VariableSliceTarget* target =
            engine.variableSlices.Target(probe.withdrawalSlice, i);
        if (target != nullptr &&
            SameLocator(target->locator, resolved.locator)) {
            return true;
        }
    }
    return false;
}

bool MeterProbeSystem::CaptureAndScheduleWithdrawalChunk(
    Engine& engine, MeterSystem::Probe& probe) {
    if (!probe.withdrawalSlice.Valid() ||
        !probe.withdrawalProjected || probe.withdrawalPageCommitted) {
        return false;
    }
    if (!probe.withdrawalSliceCaptured) {
        const std::uint32_t count =
            engine.variableSlices.TargetCount(probe.withdrawalSlice);
        for (std::uint32_t i = 0; i < count; ++i) {
            VariableSliceTarget* target =
                engine.variableSlices.Target(probe.withdrawalSlice, i);
            if (target == nullptr) {
                return false;
            }
            const std::uint32_t authoredRampMs = target->rampMs;
            float resolved = 0.0f;
            std::uint32_t rampMs = 0;
            std::uint32_t revision = 0;
            std::uint32_t effectType = target->locator.effectType;
            bool valid = false;
            switch (target->locator.scope) {
            case VariableTargetScope::Voice: {
                valid = engine.params.SnapshotForClaim(
                    static_cast<VnVoiceHandle>(target->locator.object),
                    target->locator.targetId, probe.withdrawalSlice,
                    &resolved, &rampMs, &revision);
                if (valid && target->locator.targetId == kTargetVoiceVolume &&
                    !TryVolumeGainFromDb(resolved)) {
                    engine.params.DropClaimedPublication(
                        static_cast<VnVoiceHandle>(target->locator.object),
                        target->locator.targetId, probe.withdrawalSlice);
                    ++engine.paramDropCount;
                    target->resolvedValue = resolved;
                    target->rampMs = authoredRampMs;
                    valid = true;
                    break;
                }
                float pan2d = 0.0f;
                valid = valid && engine.params.CaptureSnapshotForClaim(
                    static_cast<VnVoiceHandle>(target->locator.object),
                    target->locator.targetId, probe.withdrawalSlice,
                    &resolved, &rampMs, &revision, &pan2d);
                ParamCommit commit{};
                commit.handle =
                    static_cast<VnVoiceHandle>(target->locator.object);
                commit.targetId = target->locator.targetId;
                commit.resolved = resolved;
                commit.pan2d = pan2d;
                commit.rampMs = rampMs;
                valid = valid &&
                        MakeVariableParamCommand(engine, commit,
                                                 &target->command);
                break;
            }
            case VariableTargetScope::Bus:
                valid = engine.busTargets.SnapshotForClaim(
                    static_cast<std::uint32_t>(target->locator.object),
                    target->locator.objectGeneration,
                    target->locator.targetId, probe.withdrawalSlice,
                    &resolved, &rampMs, &revision);
                if (valid &&
                    (target->locator.targetId == kBusTargetBusVolume ||
                     target->locator.targetId == kBusTargetBusOutputVolume) &&
                    !TryVolumeGainFromDb(resolved)) {
                    engine.busTargets.DropClaimedPublication(
                        static_cast<std::uint32_t>(target->locator.object),
                        target->locator.objectGeneration,
                        target->locator.targetId, probe.withdrawalSlice);
                    ++engine.paramDropCount;
                    target->resolvedValue = resolved;
                    target->rampMs = authoredRampMs;
                    break;
                }
                valid = valid && engine.busTargets.CaptureSnapshotForClaim(
                    static_cast<std::uint32_t>(target->locator.object),
                    target->locator.objectGeneration,
                    target->locator.targetId, probe.withdrawalSlice,
                    &resolved, &rampMs, &revision);
                if (valid) {
                    const std::uint32_t busIndex =
                        static_cast<std::uint32_t>(target->locator.object);
                    const bool outputOnly =
                        target->locator.targetId == kBusTargetBusOutputVolume;
                    target->command.opcode = kOpcodeSetBusVolume;
                    target->command.payload.setBusVolume.busIndex = busIndex;
                    target->command.payload.setBusVolume.volumeDb =
                        VariableBusWireValue(engine, busIndex, resolved,
                                             target->locator.targetId);
                    target->command.payload.setBusVolume.rampMs = rampMs;
                    target->command.payload.setBusVolume.busGeneration =
                        target->locator.objectGeneration;
                    target->command.payload.setBusVolume.outputOnly =
                        outputOnly ? 1u : 0u;
                }
                break;
            case VariableTargetScope::VoiceEffect:
            case VariableTargetScope::BusEffect: {
                const EffectParamTargetStore::Scope scope =
                    target->locator.scope ==
                            VariableTargetScope::VoiceEffect
                        ? EffectParamTargetStore::Scope::Voice
                        : EffectParamTargetStore::Scope::Bus;
                valid = engine.effectParamTargets.SnapshotForClaim(
                    scope, target->locator.object,
                    target->locator.objectGeneration,
                    target->locator.effectSlot,
                    target->locator.paramIndex, probe.withdrawalSlice,
                    &resolved, &revision, &effectType);
                valid = valid && MakeVariableEffectParamCommand(
                                     engine, scope,
                                     target->locator.object,
                                     target->locator.effectSlot,
                                     target->locator.paramIndex, resolved,
                                     effectType, &target->command,
                                     authoredRampMs);
                break;
            }
            }
            if (!valid || revision != target->targetRevision ||
                effectType != target->locator.effectType) {
                return false;
            }
            if (target->command.opcode != 0) {
                target->resolvedValue = resolved;
                target->rampMs = std::max(rampMs, authoredRampMs);
            }
        }
        probe.withdrawalSliceCaptured = true;
    }
    if (probe.withdrawalLogicSequence == 0) {
        return false;
    }
    const std::uint64_t quantum = engine.timelinePool.QuantumFrames();
    const std::uint64_t blocks = engine.mixer.BlocksRendered();
    const std::uint64_t serviceFrame =
        engine.config.blockFrames == 0 ||
                blocks > UINT64_MAX / engine.config.blockFrames
            ? UINT64_MAX
            : blocks * engine.config.blockFrames;
    const std::uint64_t consumer =
        engine.timelinePool.ConsumerFrontier().load(
            std::memory_order_acquire);
    const std::uint64_t consumerFrame =
        consumer == UINT64_MAX || quantum == 0 ||
                consumer + 1u > UINT64_MAX / quantum
            ? UINT64_MAX
            : (consumer + 1u) * quantum;
    const std::uint64_t leadQuanta =
        engine.timelinePool.PublishLeadQuanta();
    const std::uint64_t lead =
        quantum != 0 && leadQuanta > UINT64_MAX / quantum
            ? UINT64_MAX
            : leadQuanta * quantum;
    const std::uint64_t leadFrame =
        engine.timelinePool.WindowStartFrame() > UINT64_MAX - lead
            ? UINT64_MAX
            : engine.timelinePool.WindowStartFrame() + lead;
    const std::uint64_t serviceNext =
        serviceFrame > UINT64_MAX - quantum ? UINT64_MAX
                                            : serviceFrame + quantum;
    const std::uint64_t nextOpenFrame = std::max(
        {engine.timelinePool.PublishCutoffFrame(), leadFrame,
         consumerFrame, serviceNext});
    ScheduledCurrentSliceInput input{};
    input.executeFrame = nextOpenFrame;
    input.logicCommitSequence = probe.withdrawalLogicSequence;
    input.kind = CurrentSliceKind::MeterProbeWithdrawal;
    input.slice = {probe.withdrawalSlice.slot,
                   probe.withdrawalSlice.generation};
    const ReserveResult reserved = engine.timelinePool.ReserveCurrentSlice(
        input, PrePublishLatePolicy::ExecuteAtEarliestSafeFrame,
        nextOpenFrame);
    if (reserved.status != ReserveStatus::Committed) {
        return false;
    }
    if (!engine.variableSlices.Publish(probe.withdrawalSlice)) {
        std::terminate();
    }
    probe.withdrawalPageCommitted = true;
    ReleaseWithdrawalChunkRows(engine, probe);
    engine.variables.PublishPreparedPlaybackPages(
        reserved.firstFrame > UINT64_MAX - quantum
            ? UINT64_MAX
            : reserved.firstFrame + quantum);
    return true;
}

void MeterProbeSystem::ReleaseWithdrawalChunkRows(
    Engine& engine, MeterSystem::Probe& probe) {
    engine.bindings.ForEachProbeAttachmentMutable(
        [&](BindingTable::Attachment& attachment,
            const BindingTable::Definition& definition) {
            if (definition.probeStableId !=
                    probe.identity.probeStableId ||
                definition.probeGeneration !=
                    probe.identity.probeGeneration) {
                return;
            }
            WithdrawalTarget resolved{};
            const bool live = attachment.lastHadValue &&
                              ResolveWithdrawalTarget(
                                  engine, attachment, definition,
                                  &resolved);
            if (!live || AttachmentInWithdrawalChunk(
                             engine, probe, attachment, definition)) {
                engine.bindings.ReleaseProbeAttachment(attachment);
            }
        });
    engine.bindings.ForEachProbeDefinitionMutable(
        [&](BindingTable::Definition& definition) {
            if (definition.probeStableId ==
                    probe.identity.probeStableId &&
                definition.probeGeneration ==
                    probe.identity.probeGeneration &&
                !engine.bindings.AnyProbeAttachmentOf(definition.id)) {
                engine.bindings.ReleaseProbeDefinition(definition);
            }
        });
    bool remaining = false;
    engine.bindings.ForEachProbeAttachment(
        [&](const BindingTable::Attachment&,
            const BindingTable::Definition& definition) {
            if (definition.probeStableId ==
                    probe.identity.probeStableId &&
                definition.probeGeneration ==
                    probe.identity.probeGeneration) {
                remaining = true;
            }
        });
    probe.withdrawalAllParticipantsReleased = !remaining;
}

void MeterProbeSystem::ReleaseSliceClaims(
    Engine& engine, VariableSliceRef slice) noexcept {
    const VariableSliceTable& slices = engine.variableSlices;
    const std::uint32_t count = slices.TargetCount(slice);
    for (std::uint32_t i = count; i > 0; --i) {
        const VariableSliceTarget* target =
            slices.Target(slice, i - 1);
        if (target == nullptr) {
            continue;
        }
        switch (target->locator.scope) {
        case VariableTargetScope::Voice:
            engine.params.ReleaseClaim(
                static_cast<VnVoiceHandle>(target->locator.object),
                target->locator.targetId, slice);
            break;
        case VariableTargetScope::Bus:
            engine.busTargets.ReleaseClaim(
                static_cast<std::uint32_t>(target->locator.object),
                target->locator.objectGeneration,
                target->locator.targetId, slice);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            engine.effectParamTargets.ReleaseClaim(
                target->locator.scope ==
                        VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target->locator.object,
                target->locator.objectGeneration,
                target->locator.effectSlot,
                target->locator.paramIndex, slice);
            break;
        }
    }
}

void MeterProbeSystem::CommitSliceTargets(
    Engine& engine, VariableSliceRef slice) noexcept {
    const VariableSliceTable& slices = engine.variableSlices;
    const std::uint32_t count = slices.TargetCount(slice);
    for (std::uint32_t i = 0; i < count; ++i) {
        const VariableSliceTarget* target =
            slices.Target(slice, i);
        if (target == nullptr) {
            continue;
        }
        if (target->command.opcode == 0) {
            switch (target->locator.scope) {
            case VariableTargetScope::Voice:
                engine.params.ReleaseClaim(
                    static_cast<VnVoiceHandle>(target->locator.object),
                    target->locator.targetId, slice);
                break;
            case VariableTargetScope::Bus:
                engine.busTargets.ReleaseClaim(
                    static_cast<std::uint32_t>(target->locator.object),
                    target->locator.objectGeneration,
                    target->locator.targetId, slice);
                break;
            case VariableTargetScope::VoiceEffect:
            case VariableTargetScope::BusEffect:
                engine.effectParamTargets.ReleaseClaim(
                    target->locator.scope ==
                            VariableTargetScope::VoiceEffect
                        ? EffectParamTargetStore::Scope::Voice
                        : EffectParamTargetStore::Scope::Bus,
                    target->locator.object,
                    target->locator.objectGeneration,
                    target->locator.effectSlot,
                    target->locator.paramIndex, slice);
                break;
            }
            continue;
        }
        switch (target->locator.scope) {
        case VariableTargetScope::Voice:
            engine.params.MarkSliceCommitted(
                static_cast<VnVoiceHandle>(target->locator.object),
                target->locator.targetId, slice,
                target->resolvedValue, target->rampMs,
                target->locator.targetId == kTargetVoicePan
                    ? target->command.payload.setVoicePan.pan2d : 0.0f);
            break;
        case VariableTargetScope::Bus:
            engine.busTargets.MarkSliceCommitted(
                static_cast<std::uint32_t>(target->locator.object),
                target->locator.objectGeneration,
                target->locator.targetId, slice,
                target->resolvedValue, target->rampMs);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            engine.effectParamTargets.MarkSliceCommitted(
                target->locator.scope ==
                        VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target->locator.object,
                target->locator.objectGeneration,
                target->locator.effectSlot,
                target->locator.paramIndex, slice,
                target->resolvedValue);
            break;
        }
    }
}

VnResult MeterProbeSystem::Install(Engine& engine,
                                   VnObjectId probeStableId,
                                   VnBusId busId, std::uint32_t tap,
                                   std::uint32_t attackMs,
                                   std::uint32_t releaseMs,
                                   std::uint32_t* installedGeneration) {
    if (probeStableId == VnObjectId{} || tap > VN_BUS_METER_TAP_POST_FADER) {
        engine.meterDetectors.NoteRefusal();
        return VN_INVALID_ARG;
    }
    if (engine.meterDetectors.Find(probeStableId) != nullptr) {
        engine.meterDetectors.NoteRefusal();
        return VN_IN_USE;
    }
    const std::uint32_t busIndex = engine.buses.FindIndex(busId);
    if (busIndex == kInvalidBusIndex) {
        engine.meterDetectors.NoteRefusal();
        return VN_UNKNOWN_ID;
    }
    const VnResult result = engine.meterDetectors.Install(
        probeStableId, busId, busIndex, engine.buses.GenerationAt(busIndex),
        tap, attackMs, releaseMs, engine.meterRing.WriteCursor(),
        installedGeneration);
    if (result == VN_OK) {
        engine.meterRing.SetArmedCount(engine.meterDetectors.ActiveCount());
    }
    return result;
}

VnResult MeterProbeSystem::DefineBinding(
    Engine& engine, VnObjectId bindingId, VnObjectId probeStableId,
    std::uint32_t probeGeneration, std::uint32_t targetScope,
    std::uint32_t targetId, const CurvePoint* points,
    const CurveHandlePair* handles, std::uint32_t pointCount,
    std::uint32_t transitionMs) {
    const MeterSystem::Probe* probe =
        engine.meterDetectors.Find(probeStableId);
    if (bindingId == VnObjectId{} || probe == nullptr ||
        probe->identity.probeGeneration != probeGeneration ||
        probe->state != MeterSystem::State::Installed) {
        engine.meterDetectors.NoteRefusal();
        if (probe == nullptr) {
            return VN_UNKNOWN_ID;
        }
        return probe->identity.probeGeneration != probeGeneration
                   ? VN_STALE_HANDLE
                   : VN_IN_USE;
    }
    if (engine.bindings.FindProbeDefinition(bindingId) != nullptr) {
        engine.meterDetectors.NoteRefusal();
        return VN_IN_USE;
    }
    if (targetScope != VN_TARGET_SCOPE_VOICE &&
        targetScope != VN_TARGET_SCOPE_BUS &&
        targetScope != VN_TARGET_SCOPE_VOICE_EFFECT_PARAM &&
        targetScope != VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
        engine.meterDetectors.NoteRefusal();
        return VN_INVALID_ARG;
    }
    if ((targetScope == VN_TARGET_SCOPE_VOICE ||
         targetScope == VN_TARGET_SCOPE_BUS) &&
        FindTargetMeta(targetScope, targetId) == nullptr) {
        engine.meterDetectors.NoteRefusal();
        return VN_INVALID_ARG;
    }
    const VnResult result = engine.bindings.RegisterProbeBinding(
        bindingId, probeStableId, probeGeneration, targetScope, targetId,
        points, handles, pointCount, transitionMs);
    if (result != VN_OK) {
        engine.meterDetectors.NoteRefusal();
    }
    return result;
}

VnResult MeterProbeSystem::UndefineBinding(Engine& engine,
                                           VnObjectId bindingId) {
    BindingTable::Definition* def =
        engine.bindings.FindProbeDefinition(bindingId);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    if (engine.bindings.AnyProbeAttachmentOf(bindingId)) {
        return VN_IN_USE;
    }
    engine.bindings.ReleaseProbeDefinition(*def);
    return VN_OK;
}

VnResult MeterProbeSystem::Attach(Engine& engine, VnObjectId bindingId,
                                  std::uint32_t targetScope,
                                  VnObjectId targetStableId, VnVoiceHandle targetInstanceHandle,
                                  std::uint32_t targetGeneration) {
    const bool busTarget = targetScope == VN_TARGET_SCOPE_BUS ||
        targetScope == VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    const bool voiceTarget = targetScope == VN_TARGET_SCOPE_VOICE ||
        targetScope == VN_TARGET_SCOPE_VOICE_EFFECT_PARAM;
    if ((!busTarget && !voiceTarget) ||
        (busTarget && targetInstanceHandle != 0) ||
        (voiceTarget && !IsNull(targetStableId)))
        return VN_INVALID_ARG;
    const BindingTable::Definition* binding =
        engine.bindings.FindProbeDefinition(bindingId);
    if (binding == nullptr) {
        engine.meterDetectors.NoteRefusal();
        return VN_UNKNOWN_ID;
    }
    const MeterSystem::Probe* probe =
        engine.meterDetectors.Find(binding->probeStableId);
    if (probe == nullptr ||
        probe->identity.probeGeneration != binding->probeGeneration ||
        probe->state != MeterSystem::State::Installed) {
        engine.meterDetectors.NoteRefusal();
        return VN_IN_USE;
    }
    if (binding->scopeKind != targetScope ||
        engine.bindings.FindProbeAttachment(bindingId, targetScope,
                                            targetStableId, targetInstanceHandle,
                                            targetGeneration) != nullptr) {
        engine.meterDetectors.NoteRefusal();
        return binding->scopeKind != targetScope ? VN_INVALID_ARG
                                                  : VN_IN_USE;
    }
    std::uint32_t busIndex = 0;
    if (targetScope == VN_TARGET_SCOPE_BUS ||
        targetScope == VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
        busIndex = engine.buses.FindIndex(targetStableId);
        if (busIndex == kInvalidBusIndex) {
            engine.meterDetectors.NoteRefusal();
            return VN_UNKNOWN_ID;
        }
        if (engine.buses.GenerationAt(busIndex) != targetGeneration) {
            engine.meterDetectors.NoteRefusal();
            return VN_STALE_HANDLE;
        }
    } else {
        const VnVoiceHandle render = engine.ledger.ToRender(targetInstanceHandle);
        if (render == 0 ||
            static_cast<std::uint32_t>(render >> 32) != targetGeneration) {
            engine.meterDetectors.NoteRefusal();
            return VN_STALE_HANDLE;
        }
    }
    const VnResult result = engine.bindings.AttachProbe(
        bindingId, targetScope, targetStableId, targetInstanceHandle, busIndex, targetGeneration);
    if (result != VN_OK) {
        engine.meterDetectors.NoteRefusal();
    }
    return result;
}

VnResult MeterProbeSystem::Detach(Engine& engine, VnObjectId bindingId,
                                  std::uint32_t targetScope,
                                  VnObjectId targetStableId, VnVoiceHandle targetInstanceHandle,
                                  std::uint32_t targetGeneration) {
    const bool busTarget = targetScope == VN_TARGET_SCOPE_BUS ||
        targetScope == VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    const bool voiceTarget = targetScope == VN_TARGET_SCOPE_VOICE ||
        targetScope == VN_TARGET_SCOPE_VOICE_EFFECT_PARAM;
    if ((!busTarget && !voiceTarget) ||
        (busTarget && targetInstanceHandle != 0) ||
        (voiceTarget && !IsNull(targetStableId)))
        return VN_INVALID_ARG;
    BindingTable::Attachment* attachment =
        engine.bindings.FindProbeAttachment(bindingId, targetScope,
                                            targetStableId, targetInstanceHandle,
                                            targetGeneration);
    if (attachment == nullptr) {
        return VN_UNKNOWN_ID;
    }
    const BindingTable::Definition& definition =
        engine.bindings.DefinitionOf(*attachment);
    const MeterSystem::Probe* probe =
        engine.meterDetectors.Find(definition.probeStableId);
    if (probe != nullptr &&
        probe->identity.probeGeneration == definition.probeGeneration &&
        probe->state == MeterSystem::State::Withdrawing) {
        return VN_IN_USE;
    }
    DetachProbeAttachment(engine, *attachment, definition);
    return VN_OK;
}

void MeterProbeSystem::RetireProbe(Engine& engine,
                                   MeterSystem::Probe& probe,
                                   std::uint64_t logicSequence) noexcept {
    if (probe.state != MeterSystem::State::Installed) {
        return;
    }
    std::uint32_t rampMs = 0;
    engine.bindings.ForEachProbeAttachment(
        [&](const BindingTable::Attachment&,
            const BindingTable::Definition& def) {
            if (def.probeStableId == probe.identity.probeStableId &&
                def.probeGeneration == probe.identity.probeGeneration) {
                rampMs = std::max(rampMs, def.transitionMs);
            }
    });
    engine.meterDetectors.BeginWithdrawal(probe, rampMs);
    probe.withdrawalLogicSequence = logicSequence;
    ++engine.meterProbeWithdrawalCount;
    engine.meterRing.SetArmedCount(engine.meterDetectors.ActiveCount());
}

VnResult MeterProbeSystem::Uninstall(Engine& engine,
                                     VnObjectId probeStableId,
                                     std::uint32_t expectedGeneration) {
    MeterSystem::Probe* probe = engine.meterDetectors.Find(probeStableId);
    if (probe == nullptr) {
        return VN_UNKNOWN_ID;
    }
    if (probe->identity.probeGeneration != expectedGeneration) {
        return VN_STALE_HANDLE;
    }
    if (probe->state != MeterSystem::State::Installed) {
        return VN_IN_USE;
    }
    std::uint64_t logicSequence = 0;
    if (engine.logicCommits.TryIssue(&logicSequence) !=
        LogicCommitCoordinator::IssueStatus::Issued) {
        engine.runtimeRecreateRequired = true;
        return VN_BACKPRESSURE;
    }
    RetireProbe(engine, *probe, logicSequence);
    return VN_OK;
}

void MeterProbeSystem::Evaluate(Engine& engine) {
    for (std::uint32_t slot = 0; slot < engine.meterDetectors.Capacity();
         ++slot) {
        MeterSystem::Probe* probe = engine.meterDetectors.RowAt(slot);
        if (probe == nullptr ||
            probe->state != MeterSystem::State::Withdrawing) {
            continue;
        }
        (void)PrepareWithdrawalChunk(engine, *probe);
        if (probe->withdrawalSlice.Valid() &&
            !probe->withdrawalProjected &&
            !probe->withdrawalPageCommitted &&
            engine.commands.SettledFrontier() >=
                probe->withdrawalChunkCommandFrontier) {
            probe->withdrawalProjected = true;
        }
    }
    auto& dependencies = engine.bindings.Dependencies();
    for (std::uint32_t slot = 0; slot < engine.meterDetectors.Capacity(); ++slot) {
        const auto* probe = engine.meterDetectors.RowAt(slot);
        if (probe == nullptr || (probe->state != MeterSystem::State::Installed &&
                                probe->state != MeterSystem::State::Withdrawing)) continue;
        BindingDependencyIdentity identity{};
        identity.source = probe->identity.probeStableId;
        identity.generation = probe->identity.probeGeneration;
        dependencies.Notify(BindingDependencyKind::Probe, identity);
    }
    engine.busTargets.BeginMeterProbeSweep();
    engine.effectParamTargets.BeginMeterProbeSweep();
    const auto evaluate = [&](BindingTable::Attachment& attachment,
                              const BindingTable::Definition& def) {
        const MeterSystem::Probe* probe =
            engine.meterDetectors.Find(def.probeStableId);
        if (probe == nullptr ||
            probe->identity.probeGeneration != def.probeGeneration) {
            return;
        }
        if (probe->state == MeterSystem::State::Withdrawing) {
            const bool selected =
                probe->withdrawalProjected &&
                AttachmentInWithdrawalChunk(engine, *probe, attachment,
                                              def);
            if (selected) {
                if (attachment.lastHadValue &&
                    attachment.scopeKind == VN_TARGET_SCOPE_BUS) {
                    engine.busTargets.NoteMeterProbeRemoved(
                        attachment.busIndex,
                        attachment.targetGeneration, def.targetId,
                        def.transitionMs);
                } else if (attachment.lastHadValue &&
                           attachment.scopeKind ==
                               VN_TARGET_SCOPE_VOICE) {
                    const VnVoiceHandle render =
                        engine.ledger.ToRender(attachment.instanceHandle);
                    if (render != 0 &&
                        static_cast<std::uint32_t>(render >> 32) ==
                            attachment.targetGeneration) {
                        engine.params.ClearMeterProbeContribution(
                            render, def.id, def.targetId,
                            def.transitionMs);
                    }
                }
                return;
            }
            if (!attachment.lastHadValue) {
                return;
            }
            WithdrawalTarget target{};
            if (!ResolveWithdrawalTarget(engine, attachment, def,
                                         &target)) {
                return;
            }
            if (attachment.scopeKind == VN_TARGET_SCOPE_BUS) {
                engine.busTargets.AccumulateMeterProbe(
                    attachment.busIndex, attachment.targetGeneration,
                    def.targetId, attachment.lastContribution, false,
                    def.transitionMs);
            } else if (attachment.scopeKind ==
                           VN_TARGET_SCOPE_VOICE_EFFECT_PARAM ||
                       attachment.scopeKind ==
                           VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
                engine.effectParamTargets.AccumulateMeterProbe(
                    attachment.scopeKind ==
                            VN_TARGET_SCOPE_VOICE_EFFECT_PARAM
                        ? EffectParamTargetStore::Scope::Voice
                        : EffectParamTargetStore::Scope::Bus,
                    target.locator.object, target.locator.effectSlot,
                    target.locator.paramIndex,
                    target.locator.effectType, def.id,
                    attachment.lastContribution);
            }
            return;
        }
        if (probe->state != MeterSystem::State::Installed) {
            return;
        }
        WithdrawalTarget target{};
        if (!ResolveWithdrawalTarget(engine, attachment, def, &target)) {
            return;
        }
        const float db =
            engine.meterDetectors.ValueDb(def.probeStableId);
        const float contribution = def.Evaluate(db);
        const bool changed = !attachment.lastHadValue ||
                             attachment.lastContribution != contribution;
        attachment.lastHadValue = true;
        attachment.lastContribution = contribution;
        if (attachment.scopeKind == VN_TARGET_SCOPE_BUS) {
            engine.busTargets.AccumulateMeterProbe(
                attachment.busIndex, attachment.targetGeneration,
                def.targetId, contribution, changed, def.transitionMs);
        } else if (attachment.scopeKind == VN_TARGET_SCOPE_VOICE) {
            const VnVoiceHandle render =
                static_cast<VnVoiceHandle>(target.locator.object);
            engine.params.SetMeterProbeContribution(
                render, def.id, def.targetId, contribution,
                def.transitionMs);
        } else {
            const EffectParamLocator loc =
                UnpackEffectParamLocator(def.targetId);
            const auto scope =
                attachment.scopeKind == VN_TARGET_SCOPE_VOICE_EFFECT_PARAM
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus;
            engine.effectParamTargets.AccumulateMeterProbe(
                scope, target.locator.object, loc.slot, loc.paramIndex,
                loc.effectType, def.id, contribution);
        }
    };
    BindingDirtyTarget target;
    for (std::uint32_t cursor = 0; dependencies.DirtyTargetAt(cursor, &target); ++cursor) {
        BindingDependencyIdentity identity;
        if (!dependencies.TargetIdentity(target, &identity) ||
            identity.detail != static_cast<std::uint32_t>(BindingSourceKind::MeterProbe)) continue;
        for (auto token = dependencies.FirstContribution(target); token.Valid();
             token = dependencies.NextContribution(token)) {
            auto& attachment = *const_cast<BindingTable::Attachment*>(
                static_cast<const BindingTable::Attachment*>(dependencies.Fact(token)));
            evaluate(attachment, *attachment.dependencyDefinition);
        }
    }
    engine.busTargets.EndMeterProbeSweep();
    engine.effectParamTargets.EndMeterProbeSweep();
    // Folding leaves the index unchanged. Confirm before withdrawal releases add new departures.
    for (std::uint32_t cursor = 0; dependencies.DirtyTargetAt(cursor, &target);) {
        BindingDependencyIdentity identity;
        if (!dependencies.TargetIdentity(target, &identity) ||
            identity.detail != static_cast<std::uint32_t>(BindingSourceKind::MeterProbe)) {
            ++cursor;
            continue;
        }
        if (identity.scope == VN_TARGET_SCOPE_BUS &&
            engine.busTargets.ActiveGeneration(static_cast<std::uint32_t>(identity.object)) == identity.generation) {
            const auto* meta = FindTargetMeta(kTargetScopeBus, identity.target);
            float accumulated = 0.0f;
            bool active = false;
            for (auto token = dependencies.FirstContribution(target); token.Valid();
                 token = dependencies.NextContribution(token)) {
                const auto& attachment = *static_cast<const BindingTable::Attachment*>(dependencies.Fact(token));
                const auto& definition = *attachment.dependencyDefinition;
                const auto* probe = engine.meterDetectors.Find(definition.probeStableId);
                if (!attachment.lastHadValue || probe == nullptr ||
                    probe->identity.probeGeneration != definition.probeGeneration ||
                    (probe->state != MeterSystem::State::Installed &&
                     probe->state != MeterSystem::State::Withdrawing) ||
                    (probe->state == MeterSystem::State::Withdrawing && probe->withdrawalProjected &&
                     AttachmentInWithdrawalChunk(engine, *probe, attachment, definition))) continue;
                accumulated = active && meta != nullptr
                    ? CombineValue(meta->domain, accumulated, attachment.lastContribution)
                    : attachment.lastContribution;
                active = true;
            }
            if (active && !std::isfinite(engine.busTargets.ReduceWithSlot(
                    static_cast<std::uint32_t>(identity.object), identity.target,
                    BusTargetStore::kSlotMeterProbeIndex, accumulated))) {
                ++cursor;
                continue;
            }
        }
        if (!dependencies.CommitTarget(target, dependencies.TargetVersion(target))) ++cursor;
    }
    for (std::uint32_t slot = 0; slot < engine.meterDetectors.Capacity();
         ++slot) {
        MeterSystem::Probe* probe = engine.meterDetectors.RowAt(slot);
        if (probe != nullptr &&
            probe->state == MeterSystem::State::Withdrawing &&
            probe->withdrawalProjected &&
            !probe->withdrawalPageCommitted) {
            (void)CaptureAndScheduleWithdrawalChunk(engine, *probe);
        }
    }
}

void MeterProbeSystem::EndService(Engine& engine) {
    const std::uint64_t blocksRendered = engine.mixer.BlocksRendered();
    for (std::uint32_t slot = 0; slot < engine.meterDetectors.Capacity();
         ++slot) {
        MeterSystem::Probe* probe = engine.meterDetectors.RowAt(slot);
        if (probe == nullptr ||
            probe->state != MeterSystem::State::Withdrawing ||
            !probe->withdrawalPageCommitted ||
            !probe->withdrawalSlice.Valid()) {
            continue;
        }
        const VariableSliceState state =
            engine.variableSlices.State(probe->withdrawalSlice);
        if (state != VariableSliceState::SliceApplied &&
            state != VariableSliceState::Invalidated) {
            continue;
        }
        const std::uint64_t appliedBlock =
            engine.variableSlices.AppliedBlockSequence(
                probe->withdrawalSlice);
        if (appliedBlock == 0) {
            continue;
        }
        if (state == VariableSliceState::SliceApplied) {
            CommitSliceTargets(engine, probe->withdrawalSlice);
        } else {
            ReleaseSliceClaims(engine, probe->withdrawalSlice);
        }
        const bool acknowledged =
            engine.variableSlices.Acknowledge(probe->withdrawalSlice);
        assert(acknowledged);
        (void)acknowledged;
        probe->withdrawalAudioFenceBlock = std::max(
            probe->withdrawalAudioFenceBlock,
            AddFence(appliedBlock,
                     engine.config.mixRate.SampleRate(),
                     engine.config.blockFrames,
                     probe->withdrawalRampMs));
        probe->withdrawalSlice = {};
        probe->withdrawalProjected = false;
        probe->withdrawalSliceCaptured = false;
        probe->withdrawalPageCommitted = false;
        probe->withdrawalChunkCommandFrontier = 0;
        if (probe->withdrawalAllParticipantsReleased) {
            probe->withdrawalSettled = true;
        }
    }
    const std::uint32_t retired =
        engine.meterDetectors.ServiceWithdrawals(blocksRendered);
    for (std::uint32_t i = 0; i < retired; ++i) {
        if (engine.meterProbeWithdrawalCount > 0) {
            --engine.meterProbeWithdrawalCount;
        }
    }
}

VnResult MeterProbeSystem::PrepareBusRemovalV1(Engine& engine,
    const std::vector<BusSlotRef>& removed, PreparedBusRemovalV1& out) {
    if (out.owner != nullptr) return VN_IN_USE;
    const auto domain = engine.projectGenerationReservationDomain;
    if (domain == nullptr || !domain->live || domain->liveEngine != &engine) return VN_IN_USE;
    try {
        out.probes.clear();
        out.attachments.clear();
        out.removed = removed;
        std::sort(out.removed.begin(), out.removed.end(), [](auto a, auto b) { return a.index < b.index; });
        for (std::size_t i = 0; i < out.removed.size(); ++i) {
            const auto ref = out.removed[i];
            if (!ref.Valid() || ref.index == 0 || !engine.buses.LogicRefMatches(ref) ||
                (i != 0 && out.removed[i - 1].index == ref.index)) return VN_INVALID_ARG;
        }
        for (std::uint32_t slot = 0; slot < engine.meterDetectors.Capacity(); ++slot) {
            const auto* probe = engine.meterDetectors.RowAt(slot);
            if (probe == nullptr || (probe->state != MeterSystem::State::Installed &&
                probe->state != MeterSystem::State::Withdrawing) ||
                !RemovalContains(out.removed, probe->sourceBusIndex, probe->identity.sourceBusGeneration)) continue;
            out.probes.push_back({slot, probe->identity, probe->state, probe->sourceBusIndex,
                probe->withdrawalLogicSequence, 0});
        }
        engine.bindings.ForEachProbeAttachmentMutable([&](auto& attachment, const auto& definition) {
            const bool release = RemovalTargetsAttachment(out.removed, attachment);
            const bool withdrawingSource = std::any_of(out.probes.begin(), out.probes.end(), [&](const auto& row) {
                return row.identity.probeStableId == definition.probeStableId &&
                    row.identity.probeGeneration == definition.probeGeneration;
            });
            if (!release && !withdrawingSource) return;
            out.attachments.push_back({&attachment, attachment, definition.id, definition.probeStableId,
                definition.probeGeneration, definition.revision, definition.transitionMs, definition.ownerCookie, release});
        });
        for (auto& row : out.probes) {
            if (row.state != MeterSystem::State::Installed) continue;
            if (engine.logicCommits.TryIssue(&row.issuedSequence) != LogicCommitCoordinator::IssueStatus::Issued) {
                if (engine.logicCommits.Exhausted()) engine.runtimeRecreateRequired = true;
                return VN_BACKPRESSURE;
            }
        }
        out.domain = domain;
        out.domainEpoch = domain->epoch;
        out.owner = &engine;
        return VN_OK;
    } catch (const std::bad_alloc&) {
        return VN_BACKPRESSURE;
    }
}

bool MeterProbeSystem::CanCommitBusRemovalV1(const Engine& engine,
    const PreparedBusRemovalV1& prepared) const noexcept {
    const auto domain = prepared.domain.lock();
    if (prepared.owner != &engine || domain == nullptr || !domain->live ||
        domain->liveEngine != &engine || domain->epoch != prepared.domainEpoch) return false;
    for (const auto ref : prepared.removed)
        if (!engine.buses.LogicRefMatches(ref)) return false;
    std::size_t cursor = 0;
    for (std::uint32_t slot = 0; slot < engine.meterDetectors.Capacity(); ++slot) {
        const auto* probe = engine.meterDetectors.RowAt(slot);
        if (probe == nullptr || (probe->state != MeterSystem::State::Installed &&
            probe->state != MeterSystem::State::Withdrawing) ||
            !RemovalContains(prepared.removed, probe->sourceBusIndex, probe->identity.sourceBusGeneration)) continue;
        if (cursor >= prepared.probes.size()) return false;
        const auto& row = prepared.probes[cursor++];
        const auto& a = probe->identity;
        const auto& b = row.identity;
        if (row.slot != slot || row.state != probe->state || row.sourceBusIndex != probe->sourceBusIndex ||
            row.withdrawalSequence != probe->withdrawalLogicSequence ||
            a.probeStableId != b.probeStableId || a.probeGeneration != b.probeGeneration ||
            a.sourceBusStableId != b.sourceBusStableId || a.sourceBusGeneration != b.sourceBusGeneration || a.tap != b.tap ||
            (row.state == MeterSystem::State::Installed && row.issuedSequence == 0)) return false;
    }
    if (cursor != prepared.probes.size()) return false;
    cursor = 0;
    bool matches = true;
    engine.bindings.ForEachProbeAttachment([&](const auto& attachment, const auto& definition) {
        const bool release = RemovalTargetsAttachment(prepared.removed, attachment);
        const bool withdrawingSource = std::any_of(prepared.probes.begin(), prepared.probes.end(), [&](const auto& row) {
            return row.identity.probeStableId == definition.probeStableId &&
                row.identity.probeGeneration == definition.probeGeneration;
        });
        if (!release && !withdrawingSource) return;
        if (cursor >= prepared.attachments.size()) { matches = false; return; }
        const auto& row = prepared.attachments[cursor++];
        matches &= row.address == &attachment && row.release == release && SameRemovalAttachment(row.expected, attachment) &&
            row.definitionId == definition.id && row.probeStableId == definition.probeStableId &&
            row.probeGeneration == definition.probeGeneration && row.definitionRevision == definition.revision &&
            row.transitionMs == definition.transitionMs && row.definitionOwnerCookie == definition.ownerCookie;
    });
    return matches && cursor == prepared.attachments.size();
}

void MeterProbeSystem::CommitBusRemovalV1(Engine& engine, PreparedBusRemovalV1& prepared) noexcept {
    assert(prepared.owner == &engine);
    for (const auto& row : prepared.attachments)
        if (row.release) engine.bindings.ReleaseProbeAttachment(*row.address);
    for (const auto& row : prepared.probes) {
        if (row.state != MeterSystem::State::Installed) continue;
        assert(row.issuedSequence != 0);
        RetireProbe(engine, *engine.meterDetectors.RowAt(row.slot), row.issuedSequence);
    }
    prepared.owner = nullptr;
}

void MeterProbeSystem::NoteBusDestroyed(Engine& engine,
                                        std::uint32_t busIndex,
                                        std::uint32_t busGeneration) {
    engine.bindings.ForEachProbeAttachmentMutable(
        [&](BindingTable::Attachment& attachment,
            const BindingTable::Definition&) {
            if ((attachment.scopeKind == VN_TARGET_SCOPE_BUS ||
                 attachment.scopeKind ==
                     VN_TARGET_SCOPE_BUS_EFFECT_PARAM) &&
                attachment.busIndex == busIndex &&
                attachment.targetGeneration == busGeneration) {
                engine.bindings.ReleaseProbeAttachment(attachment);
            }
        });
    for (std::uint32_t slot = 0; slot < engine.meterDetectors.Capacity();
         ++slot) {
        MeterSystem::Probe* probe = engine.meterDetectors.RowAt(slot);
        if (probe == nullptr ||
            probe->state != MeterSystem::State::Installed ||
            probe->identity.sourceBusGeneration != busGeneration) {
            continue;
        }
        const std::uint32_t currentIndex = engine.buses.FindIndex(
            probe->identity.sourceBusStableId);
        if (probe->sourceBusIndex == busIndex ||
            currentIndex == kInvalidBusIndex) {
            std::uint64_t logicSequence = 0;
            if (engine.logicCommits.TryIssue(&logicSequence) !=
                LogicCommitCoordinator::IssueStatus::Issued) {
                engine.runtimeRecreateRequired = true;
            }
            RetireProbe(engine, *probe, logicSequence);
        }
    }
}

void MeterProbeSystem::NoteVoiceTerminal(Engine& engine,
                                         VnVoiceHandle publicHandle,
                                         VnVoiceHandle renderHandle) {
    if (publicHandle == 0 || renderHandle == 0) {
        return;
    }
    const std::uint32_t generation =
        static_cast<std::uint32_t>(renderHandle >> 32);
    engine.bindings.ForEachProbeAttachmentMutable(
        [&](BindingTable::Attachment& attachment,
            const BindingTable::Definition& def) {
            if ((attachment.scopeKind != VN_TARGET_SCOPE_VOICE &&
                 attachment.scopeKind !=
                     VN_TARGET_SCOPE_VOICE_EFFECT_PARAM) ||
                attachment.instanceHandle != publicHandle ||
                attachment.targetGeneration != generation) {
                return;
            }
            if (attachment.scopeKind == VN_TARGET_SCOPE_VOICE &&
                attachment.lastHadValue) {
                engine.params.ClearMeterProbeContribution(
                    renderHandle, def.id, def.targetId,
                    def.transitionMs);
            }
            engine.bindings.ReleaseProbeAttachment(attachment);
        });
}

std::uint32_t MeterProbeSystem::BindingCount(
    const Engine& engine) const noexcept {
    return engine.bindings.ProbeDefinitionCount();
}

std::uint32_t MeterProbeSystem::AttachmentCount(
    const Engine& engine) const noexcept {
    return engine.bindings.ProbeAttachmentCount();
}

VariableSliceRef MeterProbeSystem::WithdrawalSliceForTests(
    const Engine& engine, VnObjectId probeStableId,
    std::uint32_t probeGeneration) const noexcept {
    const MeterSystem::Probe* probe =
        engine.meterDetectors.Find(probeStableId);
    if (probe == nullptr ||
        probe->identity.probeGeneration != probeGeneration ||
        probe->state != MeterSystem::State::Withdrawing) {
        return {};
    }
    return probe->withdrawalSlice;
}

VnResult InstallMeterProbe(Engine& engine, VnObjectId probeStableId,
                           VnBusId busId, std::uint32_t tap,
                           std::uint32_t attackMs, std::uint32_t releaseMs,
                           std::uint32_t* installedGeneration) {
    engine.statefulCallSeen = true;
    std::uint32_t generation = 0;
    const VnResult result = engine.meterProbes.Install(
        engine, probeStableId, busId, tap, attackMs, releaseMs, &generation);
    if ((engine.trace == nullptr || !engine.trace->Active()) &&
        result != VN_OK) {
        engine.traceOriginWorldProbeOnly = false;
    }
    if (installedGeneration != nullptr && result == VN_OK) {
        *installedGeneration = generation;
    }
    if (engine.trace != nullptr && engine.trace->Active()) {
        engine.trace->RecordMeterProbeInstall(
            probeStableId, busId, tap, attackMs, releaseMs, generation, result);
    }
    return result;
}

VnResult UninstallMeterProbe(Engine& engine, VnObjectId probeStableId,
                             std::uint32_t expectedGeneration) {
    engine.statefulCallSeen = true;
    if (engine.trace == nullptr || !engine.trace->Active()) {
        engine.traceOriginWorldProbeOnly = false;
    }
    const VnResult result = engine.meterProbes.Uninstall(
        engine, probeStableId, expectedGeneration);
    if (engine.trace != nullptr && engine.trace->Active()) {
        engine.trace->RecordMeterProbeUninstall(
            probeStableId, expectedGeneration, result);
    }
    return result;
}

VnResult DefineMeterProbeBinding(
    Engine& engine, VnObjectId bindingId, VnObjectId probeStableId,
    std::uint32_t probeGeneration, std::uint32_t targetScope,
    std::uint32_t targetId, const CurvePoint* points,
    const CurveHandlePair* handles, std::uint32_t pointCount,
    std::uint32_t transitionMs) {
    engine.statefulCallSeen = true;
    const VnResult result = engine.meterProbes.DefineBinding(
        engine, bindingId, probeStableId, probeGeneration, targetScope,
        targetId, points, handles, pointCount, transitionMs);
    if ((engine.trace == nullptr || !engine.trace->Active()) &&
        result != VN_OK) {
        engine.traceOriginWorldProbeOnly = false;
    }
    if (engine.trace != nullptr && engine.trace->Active()) {
        engine.trace->RecordMeterProbeBindingDefine(
            bindingId, probeStableId, probeGeneration, targetScope, targetId,
            points, handles, pointCount, transitionMs, result);
    }
    return result;
}

VnResult UndefineMeterProbeBinding(Engine& engine,
                                   VnObjectId bindingId) {
    engine.statefulCallSeen = true;
    if (engine.trace == nullptr || !engine.trace->Active()) {
        engine.traceOriginWorldProbeOnly = false;
    }
    const VnResult result =
        engine.meterProbes.UndefineBinding(engine, bindingId);
    if (engine.trace != nullptr && engine.trace->Active()) {
        engine.trace->RecordMeterProbeBindingUndefine(bindingId, result);
    }
    return result;
}

VnResult AttachMeterProbeBinding(Engine& engine, VnObjectId bindingId,
                                 std::uint32_t targetScope,
                                 VnObjectId targetStableId, VnVoiceHandle targetInstanceHandle,
                                 std::uint32_t targetGeneration) {
    engine.statefulCallSeen = true;
    const VnResult result = engine.meterProbes.Attach(
        engine, bindingId, targetScope, targetStableId, targetInstanceHandle, targetGeneration);
    if ((engine.trace == nullptr || !engine.trace->Active()) &&
        result != VN_OK) {
        engine.traceOriginWorldProbeOnly = false;
    }
    if (engine.trace != nullptr && engine.trace->Active()) {
        const MeterProbeAttachment attachment{bindingId, targetScope,
                                              targetStableId, targetInstanceHandle, targetGeneration};
        engine.trace->RecordMeterProbeBindingAttachment(
            kTraceRecordMeterProbeBindingAttach, attachment, result);
    }
    return result;
}

VnResult DetachMeterProbeBinding(Engine& engine, VnObjectId bindingId,
                                 std::uint32_t targetScope,
                                 VnObjectId targetStableId, VnVoiceHandle targetInstanceHandle,
                                 std::uint32_t targetGeneration) {
    engine.statefulCallSeen = true;
    if (engine.trace == nullptr || !engine.trace->Active()) {
        engine.traceOriginWorldProbeOnly = false;
    }
    const VnResult result = engine.meterProbes.Detach(
        engine, bindingId, targetScope, targetStableId, targetInstanceHandle, targetGeneration);
    if (engine.trace != nullptr && engine.trace->Active()) {
        const MeterProbeAttachment attachment{bindingId, targetScope,
                                              targetStableId, targetInstanceHandle, targetGeneration};
        engine.trace->RecordMeterProbeBindingAttachment(
            kTraceRecordMeterProbeBindingDetach, attachment, result);
    }
    return result;
}

} // namespace harken
