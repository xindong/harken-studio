#include "harken/render/VoicePool.h"
#include "harken/render/BuiltinEffects.h"
#include <algorithm>
#include <limits>
#include <new>

namespace harken {
namespace {
bool SameRef(const EffectInstanceRef& a, const EffectInstanceRef& b) noexcept {
    return a.type == b.type && a.poolIndex == b.poolIndex && a.generation == b.generation;
}
}

VnResult VoicePool::PrepareEffectChainUpdate(VnVoiceHandle handle,
    const std::array<PreviewEffectChainEntry, kMaxFxSlotsPerVoice>& targets,
    const std::array<EffectInstanceRef, kMaxFxSlotsPerVoice>& currentRefs,
    const std::array<std::uint8_t, kMaxFxSlotsPerVoice>& parameterMask,
    std::uint32_t bypassMask, VnObjectId projectId, EffectChainUpdate** out) noexcept {
    *out = nullptr;
    if (CheckLive(handle) != VN_OK) return VN_STALE_HANDLE;
    auto& slot = m_slots[static_cast<std::uint32_t>(handle)];
    EffectChainUpdate* update = slot.effectChainUpdate.load(std::memory_order_acquire);
    try {
        if (update == nullptr) {
            update = new EffectChainUpdate;
            slot.effectChainUpdate.store(update, std::memory_order_release);
        }
        if (update->phase.load(std::memory_order_acquire) != EffectChainUpdate::Phase::Free)
            return VN_BACKPRESSURE;
        if (update->acceptedHandle == handle && update->acceptedRevision == UINT64_MAX)
            return VN_RUNTIME_RECREATE_REQUIRED;
        update->phase.store(EffectChainUpdate::Phase::Preparing, std::memory_order_release);
        update->host = handle;
        update->baseRevision = update->acceptedHandle == handle ? update->acceptedRevision : 0;
        update->targetRevision = update->baseRevision + 1;
        update->targets = targets;
        update->targetRefs = currentRefs;
        update->oldRefs = currentRefs;
        update->oldEffects = {};
        update->oldBypass = {};
        update->ownedTargets = update->ownedOld = update->changedMask = 0;
        update->parameterMask = parameterMask;
        update->bypassMask = bypassMask;
        update->channels = slot.sourceChannels.load(std::memory_order_acquire);
        update->maxFrames = m_blockFrames;
        update->done = 0;
        update->totalFrames = std::max(1u, static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(m_sampleRate) * 5u + 999u) / 1000u));
        if (update->channels == 0 || update->channels > kMaxChannels) {
            CancelEffectChainUpdate(*update);
            return VN_STALE_HANDLE;
        }
        for (std::uint32_t i = 0; i < m_fxSlotsPerVoice; ++i) {
            const auto& target = targets[i];
            const auto& prior = currentRefs[i];
            const bool same = prior.type == target.effectType &&
                (target.effectType == kEffectTypeNone ||
                 m_effectBank.ObjectIdentity(prior).effectId == target.effectId);
            if (!same) update->changedMask |= 1u << i;
            if (target.effectType == kEffectTypeNone) {
                update->targetRefs[i] = {};
                continue;
            }
            if (!PrepareLimiterParam(kLimiterParamCeilingDb, target.ceilingDb,
                    m_sampleRate, &update->preparedParams[i][0]) ||
                !PrepareLimiterParam(kLimiterParamReleaseMs, target.releaseMs,
                    m_sampleRate, &update->preparedParams[i][1])) {
                CancelEffectChainUpdate(*update);
                return VN_INVALID_ARG;
            }
            if (same) continue;
            const auto ref = m_effectBank.Acquire(VN_EFFECT_LIMITER, {target.effectId, projectId});
            if (ref.type == kEffectTypeNone) {
                CancelEffectChainUpdate(*update);
                return VN_NO_EFFECT;
            }
            update->targetRefs[i] = ref;
            update->ownedTargets |= 1u << i;
            auto* state = m_effectBank.LiveStateFor(ref);
            auto effect = BuildEffect(ref.type, state);
            effect.reset(state);
            LimiterApplyPreparedParam(state, kLimiterParamCeilingDb, update->preparedParams[i][0]);
            LimiterApplyPreparedParam(state, kLimiterParamReleaseMs, update->preparedParams[i][1]);
        }
        if (update->changedMask != 0 || bypassMask != 0) {
            const auto samples = static_cast<std::size_t>(update->channels) * m_blockFrames;
            update->scratch.resize(samples * 2u);
            for (std::uint32_t ch = 0; ch < update->channels; ++ch) {
                update->dry[ch] = update->scratch.data() + static_cast<std::size_t>(ch) * m_blockFrames;
                update->alternate[ch] = update->scratch.data() + samples +
                    static_cast<std::size_t>(ch) * m_blockFrames;
            }
        }
        if (CheckLive(handle) != VN_OK) {
            CancelEffectChainUpdate(*update);
            return VN_STALE_HANDLE;
        }
        *out = update;
        return VN_OK;
    } catch (...) {
        if (update != nullptr && update->phase.load(std::memory_order_relaxed) ==
            EffectChainUpdate::Phase::Preparing) CancelEffectChainUpdate(*update);
        return VN_DEVICE_ERROR;
    }
}

void VoicePool::QueueEffectChainUpdate(EffectChainUpdate& update) noexcept {
    m_pendingEffectChains.fetch_add(1, std::memory_order_relaxed);
    update.phase.store(EffectChainUpdate::Phase::Queued, std::memory_order_release);
}

void VoicePool::CancelEffectChainUpdate(EffectChainUpdate& update) noexcept {
    if (update.phase.load(std::memory_order_relaxed) == EffectChainUpdate::Phase::Queued)
        m_pendingEffectChains.fetch_sub(1, std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < kMaxFxSlotsPerVoice; ++i)
        if ((update.ownedTargets & (1u << i)) != 0) m_effectBank.Recycle(update.targetRefs[i]);
    update.ownedTargets = update.ownedOld = 0;
    std::vector<float>().swap(update.scratch);
    update.dry = update.alternate = {};
    update.phase.store(EffectChainUpdate::Phase::Free, std::memory_order_release);
}

void VoicePool::ApplyEffectChainUpdate(VnVoiceHandle handle, std::uint64_t revision) noexcept {
    const auto index = static_cast<std::uint32_t>(handle);
    if (index >= m_slots.size()) return;
    auto* update = m_slots[index].effectChainUpdate.load(std::memory_order_acquire);
    if (update == nullptr || update->phase.load(std::memory_order_acquire) !=
        EffectChainUpdate::Phase::Queued || update->host != handle ||
        update->targetRevision != revision) return;
    auto* slot = SlotForCommandMetadata(handle);
    bool valid = slot != nullptr && slot->render.fxChainTransition == nullptr &&
        slot->render.fxChainRevision == update->baseRevision &&
        slot->render.pcm.channelCount == update->channels;
    for (std::uint32_t i = 0; valid && i < m_fxSlotsPerVoice; ++i) {
        valid = SameRef(slot->render.fxSlotEffectRef[i], update->oldRefs[i]) &&
            (update->targetRefs[i].type == kEffectTypeNone ||
             m_effectBank.LiveStateFor(update->targetRefs[i]) != nullptr);
    }
    if (!valid) {
        update->phase.store(EffectChainUpdate::Phase::Retiring, std::memory_order_release);
        RetireEffectChainReferences(*update, ReleaseThread::Audio);
        return;
    }
    auto& render = slot->render;
    for (std::uint32_t i = 0; i < render.fxSlotCount; ++i) {
        const auto bit = 1u << i;
        if ((update->changedMask & bit) != 0) {
            update->oldEffects[i] = render.fxSlots[i];
            update->oldBypass[i] = render.fxBypassMix[i];
            if (update->oldRefs[i].type != kEffectTypeNone) update->ownedOld |= bit;
            if (update->targetRefs[i].type == kEffectTypeNone &&
                render.fxBypassMix[i] == 1.0f && !render.fxBypassRamping[i])
                update->changedMask &= ~bit;
            render.fxSlotEffectRef[i] = update->targetRefs[i];
            render.fxSlots[i] = update->targetRefs[i].type == kEffectTypeNone ? InsertEffect{} :
                BuildEffect(update->targetRefs[i].type, m_effectBank.LiveStateFor(update->targetRefs[i]));
            render.fxBypassMix[i] = render.fxBypassTarget[i] = update->targets[i].bypass ? 1.0f : 0.0f;
            render.fxBypassRamping[i] = false;
        } else {
            update->oldRefs[i] = {};
            if (update->targetRefs[i].type == VN_EFFECT_LIMITER) {
                for (std::uint32_t parameter = 0; parameter < 2; ++parameter)
                    if ((update->parameterMask[i] & (1u << parameter)) != 0)
                        LimiterRequestPreparedParam(render.fxSlots[i].state, parameter,
                            update->preparedParams[i][parameter], update->totalFrames);
            }
            if ((update->bypassMask & bit) != 0) {
                render.fxBypassTarget[i] = update->targets[i].bypass ? 1.0f : 0.0f;
                render.fxBypassRamping[i] = render.fxSlots[i].process != nullptr &&
                    render.fxBypassMix[i] != render.fxBypassTarget[i];
                if (!render.fxSlots[i].process) render.fxBypassMix[i] = render.fxBypassTarget[i];
            }
        }
    }
    update->ownedTargets = 0; // Every target is now owned by the render chain.
    render.fxChainRevision = revision;
    if (update->changedMask != 0 || update->bypassMask != 0) {
        if (update->changedMask == 0) update->totalFrames = 1;
        render.fxChainTransition = update;
        update->phase.store(EffectChainUpdate::Phase::Active, std::memory_order_release);
    } else {
        update->phase.store(EffectChainUpdate::Phase::Retiring, std::memory_order_release);
        RetireEffectChainReferences(*update, ReleaseThread::Audio);
    }
}

void VoicePool::RetireEffectChainReferences(EffectChainUpdate& update, ReleaseThread thread) noexcept {
    const auto retire = [&](auto& refs, std::uint32_t& owned) {
        for (std::uint32_t i = 0; i < kMaxFxSlotsPerVoice; ++i) {
            const auto bit = 1u << i;
            if ((owned & bit) == 0) continue;
            if (thread == ReleaseThread::Logic) m_effectBank.Recycle(refs[i]);
            else if (!m_effectBank.Retire(refs[i])) continue;
            owned &= ~bit;
        }
    };
    retire(update.oldRefs, update.ownedOld);
    retire(update.targetRefs, update.ownedTargets);
    if (update.ownedOld == 0 && update.ownedTargets == 0)
        update.phase.store(EffectChainUpdate::Phase::Complete, std::memory_order_release);
}

void VoicePool::FinishEffectChainTransition(Slot& slot) noexcept {
    auto* update = slot.render.fxChainTransition;
    if (update == nullptr || update->done < update->totalFrames) return;
    slot.render.fxChainTransition = nullptr;
    update->phase.store(EffectChainUpdate::Phase::Retiring, std::memory_order_release);
    RetireEffectChainReferences(*update, ReleaseThread::Audio);
}

void VoicePool::ReleaseEffectChainTransition(Slot& slot, ReleaseThread thread) noexcept {
    auto* update = slot.render.fxChainTransition;
    if (update == nullptr) return;
    // ReleaseHeld or audio rendezvous excludes the cascade. Move current refs
    // to this independent retirement owner before the physical slot is reused.
    for (std::uint32_t i = 0; i < slot.render.fxSlotCount; ++i) {
        update->targetRefs[i] = slot.render.fxSlotEffectRef[i];
        if (update->targetRefs[i].type != kEffectTypeNone) update->ownedTargets |= 1u << i;
        slot.render.fxSlotEffectRef[i] = {};
        slot.render.fxSlots[i] = {};
    }
    slot.render.fxChainTransition = nullptr;
    // A logic ReleaseHeld must not expose a retirement producer to the mixer.
    if (thread == ReleaseThread::Audio)
        update->phase.store(EffectChainUpdate::Phase::Retiring, std::memory_order_release);
    RetireEffectChainReferences(*update, thread);
}

void VoicePool::ServiceEffectChainRetirements() noexcept {
    if (m_pendingEffectChains.load(std::memory_order_acquire) == 0) return;
    for (auto& slot : m_slots) {
        auto* update = slot.effectChainUpdate.load(std::memory_order_acquire);
        if (update != nullptr && update->phase.load(std::memory_order_acquire) ==
            EffectChainUpdate::Phase::Retiring) RetireEffectChainReferences(*update, ReleaseThread::Audio);
    }
}

void VoicePool::DrainEffectChainUpdates() noexcept {
    if (m_pendingEffectChains.load(std::memory_order_acquire) == 0) return;
    for (auto& slot : m_slots) {
        auto* update = slot.effectChainUpdate.load(std::memory_order_acquire);
        if (update == nullptr || update->phase.load(std::memory_order_acquire) !=
            EffectChainUpdate::Phase::Complete) continue;
        // The acquire above proves every retirement preceded this drain, even
        // when the callback completed after Vn_Update's first pool drain.
        m_effectBank.DrainRetire();
        std::vector<float>().swap(update->scratch);
        update->dry = update->alternate = {};
        update->phase.store(EffectChainUpdate::Phase::Free, std::memory_order_release);
        m_pendingEffectChains.fetch_sub(1, std::memory_order_release);
    }
}

void VoicePool::ShutdownEffectChainUpdates() noexcept {
    // Caller has joined workers and excluded callbacks, including quarantine.
    for (auto& slot : m_slots) {
        ReleaseEffectChainTransition(slot, ReleaseThread::Logic);
        auto* update = slot.effectChainUpdate.load(std::memory_order_acquire);
        if (update == nullptr) continue;
        RetireEffectChainReferences(*update, ReleaseThread::Logic);
        std::vector<float>().swap(update->scratch);
    }
}

} // namespace harken
