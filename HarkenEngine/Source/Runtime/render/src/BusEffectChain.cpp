#include "harken/render/BusTree.h"
#include "harken/render/BusActivityPlan.h"
#include "harken/render/BuiltinEffects.h"
#include "MonitorCaptureHub.h"
#include <algorithm>
#include <limits>

namespace harken {
namespace {
using Phase = EffectChainUpdate::Phase;
bool SameEntry(const PreviewEffectChainEntry& a, const PreviewEffectChainEntry& b) noexcept {
    return a.effectType == b.effectType && a.effectId == b.effectId &&
        a.bypass == b.bypass && a.ceilingDb == b.ceilingDb && a.releaseMs == b.releaseMs;
}
bool SameRef(const EffectInstanceRef& a, const EffectInstanceRef& b) noexcept {
    return a.type == b.type && a.poolIndex == b.poolIndex && a.generation == b.generation;
}
}

BusTree::~BusTree() {
    ShutdownActivities();
    ShutdownPreviewEffectChains();
    ShutdownWorkRows();
    for (std::uint32_t i = 0; i < m_capacity; ++i)
        delete m_previewEffectStates[i].load(std::memory_order_relaxed);
}

BusEffectChainState* BusTree::PreviewEffectState(BusSlotRef ref) const noexcept {
    if (!ref.Valid() || ref.index >= m_capacity) return nullptr;
    auto* state = m_previewEffectStates[ref.index].load(std::memory_order_acquire);
    return state != nullptr && state->acceptedHost == ref ? state : nullptr;
}

bool BusTree::PreviewEffectOwned(BusSlotRef ref) const noexcept {
    const auto* state = PreviewEffectState(ref);
    return state != nullptr && state->owner != 0 && LogicRefMatches(ref);
}

bool BusTree::PreviewMasterEffectsClaimedByOther(std::uint64_t owner) const noexcept {
    const auto* state = m_previewEffectStates[0].load(std::memory_order_acquire);
    if (state == nullptr) return false;
    const auto phase = state->update.phase.load(std::memory_order_acquire);
    if (phase == Phase::Preparing && state->preparedOwner == owner) return false;
    if (state->owner == owner) return false;
    if (phase != Phase::Free) return true;
    return std::any_of(state->accepted.begin(), state->accepted.end(),
        [](const auto& entry) { return entry.effectType != kEffectTypeNone; });
}

VnResult BusTree::PreparePreviewEffectChain(BusSlotRef host, std::uint64_t owner,
    const std::array<PreviewEffectChainEntry, kMaxFxSlotsPerBus>& targets,
    const std::array<EffectInstanceRef, kMaxFxSlotsPerBus>& current,
    VnObjectId projectId, PreparedBusEffectChain& out) noexcept {
    out = {};
    if (!host.Valid() || host.index >= m_capacity || owner == 0) return VN_INVALID_ARG;
    DrainPreviewEffectChains();
    auto* state = m_previewEffectStates[host.index].load(std::memory_order_acquire);
    try {
        if (state == nullptr) {
            state = new BusEffectChainState;
            m_previewEffectStates[host.index].store(state, std::memory_order_release);
        }
        const bool retained = state->acceptedHost == host;
        const bool accepted = retained && state->owner == owner;
        if (retained && state->owner != 0 && state->owner != owner) return VN_IN_USE;
        out = {state, host, state->owner, state->revision, false};
        bool identical = retained && state->owner == owner;
        for (std::uint32_t i = 0; i < m_fxSlots; ++i)
            identical = identical && SameEntry(state->accepted[i], targets[i]);
        if (identical) return VN_OK;
        auto& update = state->update;
        if (update.phase.load(std::memory_order_acquire) != Phase::Free) return VN_BACKPRESSURE;
        if (retained && state->revision == UINT64_MAX) return VN_RUNTIME_RECREATE_REQUIRED;
        update.phase.store(Phase::Preparing, std::memory_order_release);
        out.changes = true;
        state->preparedOwner = owner;
        update.host = host.Packed();
        update.baseRevision = retained ? state->revision : 0;
        update.targetRevision = update.baseRevision + 1;
        update.targets = targets;
        update.targetRefs = update.oldRefs = current;
        update.oldEffects = {};
        update.oldBypass = {};
        update.parameterMask = {};
        update.bypassMask = update.changedMask = update.ownedTargets = update.ownedOld = 0;
        update.channels = m_mixChannels;
        update.maxFrames = m_blockFrames;
        update.done = 0;
        update.totalFrames = std::max(1u, static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(m_sampleRate) * 5u + 999u) / 1000u));
        for (std::uint32_t i = 0; i < m_fxSlots; ++i) {
            const auto& target = targets[i];
            const bool same = current[i].type == target.effectType &&
                (target.effectType == kEffectTypeNone ||
                 m_effectBank.ObjectIdentity(current[i]).effectId == target.effectId);
            if (!same) update.changedMask |= 1u << i;
            if (!accepted || state->accepted[i].bypass != target.bypass)
                update.bypassMask |= 1u << i;
            if (target.effectType == kEffectTypeNone) {
                update.targetRefs[i] = {};
                continue;
            }
            if (target.effectType != VN_EFFECT_LIMITER ||
                !PrepareLimiterParam(kLimiterParamCeilingDb, target.ceilingDb, m_sampleRate,
                    &update.preparedParams[i][0]) ||
                !PrepareLimiterParam(kLimiterParamReleaseMs, target.releaseMs, m_sampleRate,
                    &update.preparedParams[i][1])) {
                CancelPreviewEffectChain(out);
                return VN_INVALID_ARG;
            }
            if (!accepted || state->accepted[i].ceilingDb != target.ceilingDb)
                update.parameterMask[i] |= 1;
            if (!accepted || state->accepted[i].releaseMs != target.releaseMs)
                update.parameterMask[i] |= 2;
            if (same) continue;
            const auto ref = m_effectBank.Acquire(VN_EFFECT_LIMITER, {target.effectId, projectId});
            if (ref.type == kEffectTypeNone) {
                CancelPreviewEffectChain(out);
                return VN_NO_EFFECT;
            }
            update.targetRefs[i] = ref;
            update.ownedTargets |= 1u << i;
            auto* dsp = m_effectBank.LiveStateFor(ref);
            auto effect = BuildEffect(ref.type, dsp);
            effect.reset(dsp);
            LimiterApplyPreparedParam(dsp, kLimiterParamCeilingDb, update.preparedParams[i][0]);
            LimiterApplyPreparedParam(dsp, kLimiterParamReleaseMs, update.preparedParams[i][1]);
        }
        if (update.changedMask != 0) {
            const auto samples = static_cast<std::size_t>(m_mixChannels) * m_blockFrames;
            update.scratch.resize(samples * 2);
            for (std::uint32_t ch = 0; ch < m_mixChannels; ++ch) {
                update.dry[ch] = update.scratch.data() + static_cast<std::size_t>(ch) * m_blockFrames;
                update.alternate[ch] = update.dry[ch] + samples;
            }
        }
        return VN_OK;
    } catch (...) {
        CancelPreviewEffectChain(out);
        return VN_DEVICE_ERROR;
    }
}

VnResult BusTree::PreparePreviewEffectOwnerRelease(BusSlotRef host, std::uint64_t owner,
    PreparedBusEffectChain& out) noexcept {
    out = {};
    DrainPreviewEffectChains();
    auto* state = PreviewEffectState(host);
    if (state == nullptr || state->owner == 0) return VN_OK;
    if (state->owner != owner) return VN_IN_USE;
    auto& update = state->update;
    if (update.phase.load(std::memory_order_acquire) != Phase::Free) return VN_BACKPRESSURE;
    if (state->revision == UINT64_MAX) return VN_RUNTIME_RECREATE_REQUIRED;
    out = {state, host, state->owner, state->revision, true, true};
    state->preparedOwner = owner;
    update.host = host.Packed();
    update.baseRevision = state->revision;
    update.targetRevision = state->revision + 1;
    update.ownedTargets = update.ownedOld = 0;
    update.phase.store(Phase::Preparing, std::memory_order_release);
    return VN_OK;
}

bool BusTree::CanCommitPreviewEffectChain(const PreparedBusEffectChain& prepared) const noexcept {
    const auto* state = prepared.state;
    return state != nullptr && state->owner == prepared.expectedOwner &&
        state->revision == prepared.expectedRevision &&
        (!prepared.changes || (state->update.phase.load(std::memory_order_acquire) == Phase::Preparing &&
            state->update.host == prepared.host.Packed()));
}

void BusTree::CancelPreviewEffectChain(PreparedBusEffectChain& prepared) noexcept {
    if (prepared.changes && prepared.state != nullptr) {
        auto& update = prepared.state->update;
        if (update.phase.load(std::memory_order_acquire) == Phase::Preparing &&
            update.host == prepared.host.Packed()) {
            for (std::uint32_t i = 0; i < m_fxSlots; ++i)
                if ((update.ownedTargets & (1u << i)) != 0) m_effectBank.Recycle(update.targetRefs[i]);
            update.ownedTargets = 0;
            std::vector<float>().swap(update.scratch);
            update.dry = update.alternate = {};
            update.phase.store(Phase::Free, std::memory_order_release);
        }
    }
    prepared = {};
}

void BusTree::CommitPreviewEffectChain(const PreparedBusEffectChain& prepared) noexcept {
    if (!prepared.changes) return;
    auto& state = *prepared.state;
    auto& update = state.update;
    state.acceptedHost = prepared.host;
    state.owner = prepared.releaseOwner ? 0 : state.preparedOwner;
    state.revision = update.targetRevision;
    state.accepted = prepared.releaseOwner ? decltype(state.accepted){} : update.targets;
    m_pendingPreviewEffects.fetch_add(1, std::memory_order_relaxed);
    update.phase.store(Phase::Queued, std::memory_order_release);
}

void BusTree::ApplyPreviewEffectChain(const BusEffectChainAdoption& adoption) noexcept {
    if (adoption.state == nullptr) return;
    auto& state = *adoption.state;
    auto& update = state.update;
    if (update.phase.load(std::memory_order_acquire) != Phase::Queued) return;
    if (adoption.host != update.host || adoption.owner != state.preparedOwner ||
        adoption.revision != update.targetRevision) return;
    const auto host = BusSlotRef::FromPacked(update.host);
    if (!host.Valid() || !HasWorkRow(host.index)) return;
    auto& bus = WorkState(host.index);
    if (adoption.releaseOwner) {
        // The following cold Set/Clear commands retain sole ownership of refs.
        // This adoption changes only metadata, after a quiescent preflight.
        if (bus.generation == host.generation && bus.fxChainTransition == nullptr &&
            bus.previewEffectOwner == adoption.owner && bus.previewEffectRevision == update.baseRevision) {
            bus.previewEffectOwner = 0;
            bus.previewEffectRevision = update.targetRevision;
        }
        update.phase.store(Phase::Complete, std::memory_order_release);
        return;
    }
    // Deleted hosts adopt their empty chain after the new topology is installed.
    bool valid = bus.generation == host.generation && bus.id != VnBusId{};
    valid = valid && bus.fxChainTransition == nullptr && bus.previewEffectRevision == update.baseRevision &&
        (bus.previewEffectOwner == 0 || bus.previewEffectOwner == state.preparedOwner);
    for (std::uint32_t i = 0; valid && i < m_fxSlots; ++i)
        valid = SameRef(bus.fxSlotEffectRef[i], update.oldRefs[i]);
    if (!valid) {
        update.phase.store(Phase::Retiring, std::memory_order_release);
        RetirePreviewEffectReferences(update);
        return;
    }
    if (update.changedMask != 0) {
        for (std::uint32_t i = 0; i < m_fxSlots; ++i) {
            update.oldBypass[i] = bus.fxBypassMix[i];
            if ((update.changedMask & (1u << i)) == 0 &&
                bus.fxSlotEffectRef[i].type == VN_EFFECT_LIMITER) {
                update.oldLimiterStates[i] = *static_cast<LimiterState*>(bus.fxSlots[i].state);
                update.oldEffects[i] = BuildEffect(VN_EFFECT_LIMITER, &update.oldLimiterStates[i]);
            }
        }
    }
    for (std::uint32_t i = 0; i < m_fxSlots; ++i) {
        const auto bit = 1u << i;
        if ((update.changedMask & bit) != 0) {
            update.oldEffects[i] = bus.fxSlots[i];
            update.oldBypass[i] = bus.fxBypassMix[i];
            if (update.oldRefs[i].type != kEffectTypeNone) update.ownedOld |= bit;
            if (update.targetRefs[i].type == kEffectTypeNone && bus.fxBypassMix[i] == 1 &&
                !bus.fxBypassRamping[i]) update.changedMask &= ~bit;
            if (update.oldRefs[i].type != kEffectTypeNone && monitor::AudioLiveArmed()) {
                const auto effectHandle = PackedMonitorEffectHandle(host.index, i);
                const auto effectGeneration = update.oldRefs[i].generation;
                monitor::NoteExactInstanceRetired(
                    VN_MONITOR_TAP_EFFECT_SLOT_INPUT, effectHandle, effectGeneration);
                monitor::NoteExactInstanceRetired(
                    VN_MONITOR_TAP_EFFECT_SLOT_OUTPUT, effectHandle, effectGeneration);
            }
            bus.fxSlotEffectRef[i] = update.targetRefs[i];
            bus.fxSlots[i] = update.targetRefs[i].type == kEffectTypeNone ? InsertEffect{} :
                BuildEffect(update.targetRefs[i].type, m_effectBank.LiveStateFor(update.targetRefs[i]));
            bus.fxBypassMix[i] = bus.fxBypassTarget[i] = update.targets[i].bypass ? 1.0f : 0.0f;
            bus.fxBypassRamping[i] = false;
        } else {
            update.oldRefs[i] = {};
            for (std::uint32_t parameter = 0; parameter < 2; ++parameter)
                if ((update.parameterMask[i] & (1u << parameter)) != 0)
                    LimiterRequestPreparedParam(bus.fxSlots[i].state, parameter,
                        update.preparedParams[i][parameter], update.totalFrames);
            if ((update.bypassMask & bit) != 0)
                ApplySetBusEffectBypass(host.index, i, update.targets[i].bypass, kEffectActionBypass, VN_EFFECT_LIMITER, host.generation);
        }
    }
    update.ownedTargets = 0;
    bus.previewEffectOwner = state.preparedOwner;
    bus.previewEffectRevision = update.targetRevision;
    if (update.changedMask != 0) {
        bus.fxChainTransition = &update;
        update.phase.store(Phase::Active, std::memory_order_release);
    } else {
        update.phase.store(Phase::Retiring, std::memory_order_release);
        RetirePreviewEffectReferences(update);
    }
}

void BusTree::ApplyPreviewEffectControl(BusSlotRef host, std::uint64_t owner,
    std::uint64_t revision, std::uint32_t slot, std::uint32_t parameter,
    float prepared, std::uint32_t frames, bool bypass) noexcept {
    if (!AudioRefMatches(host) || slot >= m_fxSlots) return;
    auto& bus = WorkState(host.index);
    if (bus.previewEffectOwner != owner || revision != bus.previewEffectRevision + 1 ||
        bus.fxSlotEffectRef[slot].type != VN_EFFECT_LIMITER) return;
    if (bypass) ApplySetBusEffectBypass(host.index, slot, parameter != 0, kEffectActionBypass, VN_EFFECT_LIMITER, host.generation);
    else if (parameter < 2) LimiterRequestPreparedParam(bus.fxSlots[slot].state, parameter, prepared, frames);
    else return;
    bus.previewEffectRevision = revision;
}

void BusTree::RetirePreviewEffectReferences(EffectChainUpdate& update, bool quiescent) noexcept {
    const auto retire = [&](auto& refs, auto& owned) {
        for (std::uint32_t i = 0; i < m_fxSlots; ++i) {
            const auto bit = 1u << i;
            if ((owned & bit) == 0) continue;
            if (quiescent) m_effectBank.Recycle(refs[i]);
            else if (!m_effectBank.Retire(refs[i])) continue;
            owned &= ~bit;
        }
    };
    retire(update.oldRefs, update.ownedOld);
    retire(update.targetRefs, update.ownedTargets);
    if (update.ownedOld == 0 && update.ownedTargets == 0)
        update.phase.store(Phase::Complete, std::memory_order_release);
}

void BusTree::FinishPreviewEffectChain(Bus& bus, std::uint32_t frames) noexcept {
    auto* update = bus.fxChainTransition;
    if (update == nullptr) return;
    update->done += std::min(frames, update->totalFrames - update->done);
    if (update->done != update->totalFrames) return;
    bus.fxChainTransition = nullptr;
    update->phase.store(Phase::Retiring, std::memory_order_release);
    RetirePreviewEffectReferences(*update);
}

void BusTree::ReleasePreviewEffectChain(Bus& bus, bool quiescent) noexcept {
    // The exact transition is borrowed by this work incarnation. The stable
    // Bus slot may already name a newer owner's state when this row retires.
    if (bus.fxChainTransition == nullptr) return;
    auto& update = *bus.fxChainTransition;
    const auto phase = update.phase.load(std::memory_order_acquire);
    // A published replacement has not adopted yet. Ordinary topology swaps
    // cannot remove its host before the queued adoption command is consumed.
    if (phase != Phase::Active && phase != Phase::Retiring) return;
    for (std::uint32_t i = 0; i < m_fxSlots; ++i) {
        update.targetRefs[i] = bus.fxSlotEffectRef[i];
        if (update.targetRefs[i].type != kEffectTypeNone) update.ownedTargets |= 1u << i;
        bus.fxSlotEffectRef[i] = {};
        bus.fxSlots[i] = {};
    }
    bus.fxChainTransition = nullptr;
    bus.previewEffectOwner = bus.previewEffectRevision = 0;
    update.phase.store(Phase::Retiring, std::memory_order_release);
    RetirePreviewEffectReferences(update, quiescent);
}

void BusTree::ForgetPreviewEffectOwner(BusSlotRef ref) noexcept {
    if (auto* state = PreviewEffectState(ref)) state->owner = 0;
}

void BusTree::ServicePreviewEffectRetirements() noexcept {
    if (m_pendingPreviewEffects.load(std::memory_order_acquire) == 0) return;
    for (std::uint32_t i = 0; i < m_capacity; ++i) {
        auto* state = m_previewEffectStates[i].load(std::memory_order_acquire);
        if (state != nullptr && state->update.phase.load(std::memory_order_acquire) == Phase::Retiring)
            RetirePreviewEffectReferences(state->update);
    }
}

void BusTree::DrainPreviewEffectChains() noexcept {
    if (m_pendingPreviewEffects.load(std::memory_order_acquire) == 0) return;
    for (std::uint32_t i = 0; i < m_capacity; ++i) {
        auto* state = m_previewEffectStates[i].load(std::memory_order_acquire);
        if (state == nullptr || state->update.phase.load(std::memory_order_acquire) != Phase::Complete) continue;
        m_effectBank.DrainRetire();
        std::vector<float>().swap(state->update.scratch);
        state->update.dry = state->update.alternate = {};
        state->update.phase.store(Phase::Free, std::memory_order_release);
        m_pendingPreviewEffects.fetch_sub(1, std::memory_order_release);
    }
}

void BusTree::ShutdownPreviewEffectChains() noexcept {
    // Engine destruction excludes callbacks before this owner is destroyed.
    for (std::uint32_t i = 0; i < m_capacity; ++i) {
        auto* state = m_previewEffectStates[i].load(std::memory_order_acquire);
        if (state == nullptr) continue;
        RetirePreviewEffectReferences(state->update, true);
    }
}

} // namespace harken
