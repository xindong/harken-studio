#include "harken/param/ParamSystem.h"

#include "harken/param/BindingTable.h"
#include "harken/param/EmitterTable.h"
#include "harken/param/ParamTargetTable.h"

#include <cassert>
#include <exception>
#include <type_traits>
#include <utility>

namespace harken {

namespace {

// The parameter system is keyed by the internal render-slot handle, not the
// public ledger handle a caller holds: the API layer translates public to
// render-slot before reaching here, and commits travel to the audio thread
// under the same render-slot handle. A render-slot handle packs the slot
// index in its low 32 bits; states resolve by slot and verify the full
// handle, so a reused render slot never inherits a dead voice's contributions.
std::uint32_t SlotOf(VnVoiceHandle handle) {
    return static_cast<std::uint32_t>(handle);
}

// The per-voice instance ceiling, resolved against the compile-time
// POD-layout bound (a zero config field falls back to the default bound;
// ResolveConfig owns public validation when the field goes public).
std::uint32_t ResolveInstanceSlots(const EngineConfig& config) {
    const std::uint32_t slots = config.voiceBindingInstanceSlots;
    if (slots == 0 || slots > kMaxVoiceBindingInstanceSlots) {
        return kMaxVoiceBindingInstanceSlots;
    }
    return slots;
}

} // namespace

ParamSystem::ParamSystem(const EngineConfig& config)
    : m_voiceStates(config.voiceCapacity.Render()),
      m_retiringTargets(config.maxVariableSliceTargetsTotal),
      m_sampleRate(config.mixRate.SampleRate()),
      m_variables(kMaxVariables),
      // The instance pool: Render Voice capacity times
      // voiceBindingInstanceSlots, shared by raw bindings and
      // definition-backed instances
      // (the historical fixed table is superseded by this config-driven
      // bound).
      m_bindings(static_cast<std::size_t>(config.voiceCapacity.Render()) *
                 ResolveInstanceSlots(config)),
      m_meterProbeContributions(config.maxBindingAttachments),
      m_instanceSlotsPerVoice(ResolveInstanceSlots(config)) {}

void ParamSystem::OnVoiceStarted(VnVoiceHandle handle, float baseVolumeDb,
                                 double basePitchRatio,
                                 VnEmitterHandle emitterHandle) {
    const std::uint32_t slot = SlotOf(handle);
    if (slot >= m_voiceStates.size()) {
        return;
    }
    VoiceState& state = m_voiceStates[slot];
    if (state.handle != 0 && state.handle != handle && !state.retired) {
        RetireVoiceTargets(state.handle);
    }
    // Slot reuse releases the previous occupant's worst-case demand: its
    // bindings are inert from here on (StateFor no longer matches their
    // handle), so their unique targets leave the worst-case tick — the
    // symmetric shrink to BindVoiceTarget's growth.
    if (m_capacityPublisher != nullptr && state.handle != 0 &&
        state.handle != handle) {
        m_capacityPublisher->ShrinkWorstCase(UniqueBoundTargets(state.handle));
    }
    // The wholesale reset below discards the old occupant's demand marks
    // without passing through the transition helpers; keep the live
    // census exact by releasing both marks here.
    for (std::uint32_t t = 0; t < kBuiltinTargetCount; ++t) {
        ClearTargetPendingInput(state.targets[t]);
        SetTargetClean(state.targets[t]);
    }
    // Slot reuse reclaims the dead occupant's binding entries: they are
    // inert forever (their handle can never match again), park capture
    // already copied or identified what a parked voice needs, and
    // reclaiming here is what makes the instance pool a per-voice-slots
    // bound instead of a session-lifetime leak. Order-preserving removal
    // keeps the surviving bindings' accumulation order (and therefore
    // float summation order) stable.
    if (state.handle != 0 && state.handle != handle) {
        std::uint32_t write = 0;
        for (std::uint32_t read = 0; read < m_bindingCount; ++read) {
            if (m_bindings[read].handle == state.handle) {
                continue;
            }
            if (write != read) {
                m_bindings[write] = m_bindings[read];
            }
            ++write;
        }
        m_bindingCount = write;
        for (MeterProbeContribution& contribution :
             m_meterProbeContributions) {
            if (contribution.used && contribution.handle == state.handle) {
                contribution = MeterProbeContribution{};
            }
        }
    }
    state = VoiceState{};
    state.handle = handle;
    state.emitterHandle = emitterHandle;
    state.base[kTargetVoiceVolume] = baseVolumeDb;
    state.base[kTargetVoicePitch] =
        static_cast<float>(RatioToCents(basePitchRatio));
    state.base[kTargetVoiceLowpassCutoff] = kLowpassOpenHz;
    state.base[kTargetVoiceHighpassCutoff] = kHighpassClosedHz;
    // Pan base 0 is center; width base 0 is a point source (fully panned);
    // low-shelf base 0 dB is exact bypass.
    state.base[kTargetVoicePan] = 0.0f;
    state.base[kTargetVoiceWidth] = 0.0f;
    state.base[kTargetVoiceLowShelfGainDb] = 0.0f;
    state.base[kTargetVoiceLowpassQ] = kToneDefaultQ;
    state.base[kTargetVoiceHighpassQ] = kToneDefaultQ;
    state.base[kTargetVoiceLowShelfFreqHz] = kToneLowShelfDefaultHz;
    state.base[kTargetVoiceLowShelfQ] = kToneDefaultQ;
    state.base[kTargetVoiceHighShelfGainDb] = 0.0f;
    state.base[kTargetVoiceHighShelfFreqHz] = kToneHighShelfDefaultHz;
    state.base[kTargetVoiceHighShelfQ] = kToneDefaultQ;
    for (std::uint32_t t = 0; t < kBuiltinTargetCount; ++t) {
        // With every slot inactive the reduction returns the base, so the
        // authored value is the changed-only reference point: a binding
        // that resolves back to base commits nothing.
        state.targets[t].lastCommitted = state.base[t];
    }
}

void ParamSystem::RetireVoiceTargets(VnVoiceHandle handle) noexcept {
    const std::uint32_t slot = SlotOf(handle);
    if (handle == 0 || slot >= m_voiceStates.size()) {
        return;
    }
    VoiceState& state = m_voiceStates[slot];
    if (state.handle != handle || state.retired) {
        return;
    }
    for (std::uint32_t targetId = 0; targetId < kBuiltinTargetCount;
         ++targetId) {
        TargetState& target = state.targets[targetId];
        if (!target.preparedOwner.Valid()) {
            continue;
        }
        RetiringTarget* retiring = nullptr;
        for (RetiringTarget& candidate : m_retiringTargets) {
            if (!candidate.used) {
                retiring = &candidate;
                break;
            }
        }
        if (retiring == nullptr) {
            std::terminate();
        }
        retiring->used = true;
        retiring->handle = handle;
        retiring->targetId = targetId;
        retiring->owner = target.preparedOwner;
        retiring->revision = target.revision;
        retiring->frozenResolved = Reduce(state, targetId);
        retiring->frozenPan2d = targetId == kTargetVoicePan ? Pan2dOf(state) : 0.0f;
        retiring->frozenRampMs = target.claimSnapshotCaptured
                                     ? target.preparedRampMs
                                     : target.pendingRampMs;
        retiring->claimSnapshotCaptured =
            target.claimSnapshotCaptured;
        target.suppressClaimRamp = false;
        target.claimSnapshotCaptured = false;
        target.preCandidatePendingRampMs = 0;
        target.preCandidateApiRampMs = 0;
        target.preCandidateApiRampTick = 0;
        target.candidateBaselineValid = false;
        target.preparedOwner = {};
    }
    for (std::uint32_t targetId = 0; targetId < kBuiltinTargetCount;
         ++targetId) {
        ClearTargetPendingInput(state.targets[targetId]);
        SetTargetClean(state.targets[targetId]);
    }
    state.anyDirty = false;
    state.emitterHandle = 0;
    state.retired = true;
}

void ParamSystem::ApplyTonePreset(VnVoiceHandle handle,
                                  const VoiceTonePreset& preset) {
    VoiceState* state = StateFor(handle);
    if (state == nullptr) {
        return;
    }
    const auto setBase = [this, state](std::uint32_t targetId, float value) {
        const float before = Reduce(*state, targetId);
        state->base[targetId] = value;
        if (Reduce(*state, targetId) != before) {
            SetTargetDirty(*state, state->targets[targetId]);
        }
    };
    setBase(kTargetVoiceLowpassCutoff,
            preset.lowpassEnabled ? preset.lowpassFrequencyHz
                                  : kLowpassOpenHz);
    setBase(kTargetVoiceLowpassQ, preset.lowpassQ);
    setBase(kTargetVoiceHighpassCutoff,
            preset.highpassEnabled ? preset.highpassFrequencyHz
                                   : kHighpassClosedHz);
    setBase(kTargetVoiceHighpassQ, preset.highpassQ);
    setBase(kTargetVoiceLowShelfGainDb,
            preset.lowShelfEnabled ? preset.lowShelfGainDb : 0.0f);
    // The low-shelf corner and Q are shared with the near-field proximity
    // shelf: only an ENABLED authored low shelf claims that ownership. A
    // preset with the section disabled leaves the corner to
    // ApplyProximityFrequency, so an authored proximity corner is not
    // silently overridden by a section that renders nothing.
    if (preset.lowShelfEnabled) {
        setBase(kTargetVoiceLowShelfFreqHz, preset.lowShelfFrequencyHz);
        setBase(kTargetVoiceLowShelfQ, preset.lowShelfQ);
        state->lowShelfFrequencyAuthoredByTone = true;
    }
    setBase(kTargetVoiceHighShelfGainDb,
            preset.highShelfEnabled ? preset.highShelfGainDb : 0.0f);
    setBase(kTargetVoiceHighShelfFreqHz, preset.highShelfFrequencyHz);
    setBase(kTargetVoiceHighShelfQ, preset.highShelfQ);
}

void ParamSystem::ApplyProximityFrequency(VnVoiceHandle handle,
                                          float frequencyHz) {
    VoiceState* state = StateFor(handle);
    if (state == nullptr || state->lowShelfFrequencyAuthoredByTone) {
        return;
    }
    const std::uint32_t targetId = kTargetVoiceLowShelfFreqHz;
    const float before = Reduce(*state, targetId);
    state->base[targetId] = frequencyHz;
    if (Reduce(*state, targetId) != before) {
        SetTargetDirty(*state, state->targets[targetId]);
    }
}

float ParamSystem::SetApiDirect(VnVoiceHandle handle, std::uint32_t targetId,
                                float domainValue, std::uint32_t rampMs) {
    VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return domainValue;
    }
    TargetState& target = state->targets[targetId];
    if (target.preparedOwner.Valid()) {
        return Reduce(*state, targetId);
    }
    target.slots[kSlotApiDirect] = domainValue;
    target.active[kSlotApiDirect] = true;
    // Record the API-direct change for this tick's ramp aggregation
    // (section 9.2): if binding contributions on this target also change
    // before the flush, its single published command carries the max of
    // their transitions and this rampMs. Several same-tick setters fold
    // by max too — order-free like the rest of the aggregation.
    if (target.apiRampTick == m_flushTick) {
        if (rampMs > target.apiRampMs) {
            target.apiRampMs = rampMs;
        }
    } else {
        target.apiRampMs = rampMs;
        target.apiRampTick = m_flushTick;
    }
    const float resolved = Reduce(*state, targetId);
    // The caller commits this value in the same API call (admission was
    // pre-checked), so it is the new changed-only reference and the
    // call's rampMs is the attribution record of the committed command.
    RecordPublishedValue(*state, targetId, resolved, Pan2dOf(*state));
    SetTargetClean(target);
    target.lastPublishedRampMs = rampMs;
    return resolved;
}

bool ParamSystem::PendingPublication(VnVoiceHandle handle,
                                     std::uint32_t targetId) const {
    const VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return false;
    }
    const TargetState& target = state->targets[targetId];
    return target.dirty || target.pendingInput;
}

float ParamSystem::AuthoredBase(VnVoiceHandle handle, std::uint32_t targetId) const noexcept {
    const VoiceState* state = StateFor(handle);
    return state == nullptr || targetId >= kBuiltinTargetCount ? 0.0f : state->base[targetId];
}

float ParamSystem::LastCommitted(VnVoiceHandle handle,
                                 std::uint32_t targetId) const noexcept {
    const VoiceState* state = StateFor(handle);
    return state == nullptr || targetId >= kBuiltinTargetCount
               ? 0.0f
               : state->targets[targetId].lastCommitted;
}

void ParamSystem::SetMeterProbeContribution(
    VnVoiceHandle handle, VnObjectId bindingId, std::uint32_t targetId,
    float value, std::uint32_t transitionMs) {
    VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return;
    }
    MeterProbeContribution* selected = nullptr;
    for (MeterProbeContribution& contribution : m_meterProbeContributions) {
        if (contribution.used && contribution.handle == handle &&
            contribution.bindingId == bindingId) {
            selected = &contribution;
            break;
        }
        if (!contribution.used && selected == nullptr) {
            selected = &contribution;
        }
    }
    if (selected == nullptr) {
        return;
    }
    const bool changed = !selected->used || selected->targetId != targetId ||
                         selected->value != value;
    selected->used = true;
    selected->handle = handle;
    selected->bindingId = bindingId;
    selected->targetId = targetId;
    selected->value = value;
    if (!changed) {
        return;
    }
    TargetState& target = state->targets[targetId];
    const TargetMeta* meta = FindTargetMeta(kTargetScopeVoice, targetId);
    float accumulated = meta != nullptr ? meta->neutral : 0.0f;
    bool active = false;
    for (const MeterProbeContribution& contribution :
         m_meterProbeContributions) {
        if (contribution.used && contribution.handle == handle &&
            contribution.targetId == targetId) {
            accumulated = active && meta != nullptr
                              ? CombineValue(meta->domain, accumulated,
                                             contribution.value)
                              : contribution.value;
            active = true;
        }
    }
    target.slots[kSlotMeterProbe] = accumulated;
    target.active[kSlotMeterProbe] = active;
    if (!target.suppressClaimRamp) {
        target.pendingRampMs = std::max(target.pendingRampMs, transitionMs);
    }
    SetTargetDirty(*state, target);
}

void ParamSystem::ClearMeterProbeContribution(
    VnVoiceHandle handle, VnObjectId bindingId, std::uint32_t targetId,
    std::uint32_t transitionMs) {
    VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return;
    }
    bool removed = false;
    for (MeterProbeContribution& contribution : m_meterProbeContributions) {
        if (contribution.used && contribution.handle == handle &&
            contribution.bindingId == bindingId) {
            contribution = MeterProbeContribution{};
            removed = true;
        }
    }
    if (!removed) {
        return;
    }
    TargetState& target = state->targets[targetId];
    const TargetMeta* meta = FindTargetMeta(kTargetScopeVoice, targetId);
    float accumulated = meta != nullptr ? meta->neutral : 0.0f;
    bool active = false;
    for (const MeterProbeContribution& contribution :
         m_meterProbeContributions) {
        if (contribution.used && contribution.handle == handle &&
            contribution.targetId == targetId) {
            accumulated = active && meta != nullptr
                              ? CombineValue(meta->domain, accumulated,
                                             contribution.value)
                              : contribution.value;
            active = true;
        }
    }
    target.slots[kSlotMeterProbe] = accumulated;
    target.active[kSlotMeterProbe] = active;
    if (!target.suppressClaimRamp) {
        target.pendingRampMs = std::max(target.pendingRampMs, transitionMs);
    }
    SetTargetDirty(*state, target);
}

void ParamSystem::SetAttenuationSlot(VnVoiceHandle handle,
                                     std::uint32_t targetId,
                                     float domainValue,
                                     std::uint32_t rampMs) {
    VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return;
    }
    TargetState& target = state->targets[targetId];
    if (target.active[kSlotAttenuation] &&
        target.slots[kSlotAttenuation] == domainValue) {
        return; // unchanged spatial fact: nothing to re-resolve
    }
    target.slots[kSlotAttenuation] = domainValue;
    target.active[kSlotAttenuation] = true;
    if (!target.suppressClaimRamp && rampMs > target.pendingRampMs) {
        target.pendingRampMs = rampMs;
    }
    SetTargetDirty(*state, target);
}

void ParamSystem::ClearAttenuationSlot(VnVoiceHandle handle,
                                       std::uint32_t targetId) {
    VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return;
    }
    TargetState& target = state->targets[targetId];
    if (!target.active[kSlotAttenuation]) {
        return;
    }
    target.active[kSlotAttenuation] = false;
    target.slots[kSlotAttenuation] = 0.0f;
    SetTargetDirty(*state, target);
}

VnResult ParamSystem::SetValue(VnVariableId id, float value) {
    if (!std::isfinite(value)) {
        return VN_INVALID_ARG;
    }
    Variable* variable = FindVariable(id);
    if (variable == nullptr) {
        if (m_variableCount >= kMaxVariables) {
            return VN_NO_VARIABLE;
        }
        variable = &m_variables[m_variableCount++];
        variable->id = id;
        variable->value = value;
        // First-ever value: bindings on this Variable become evaluable.
        NoteVariableWrite(id);
        return VN_OK;
    }
    // Evaluation stays the change-detection authority (it compares each
    // binding's resolved input); the write-time marking below only grows
    // the live dirty census so an active deferral's reservation extends
    // the moment the change lands, not a tick later. An unchanged value
    // marks nothing.
    if (variable->value != value) {
        variable->value = value;
        NoteVariableWrite(id);
    }
    return VN_OK;
}

VnResult ParamSystem::SetInstalledValue(VnVariableId id, float value) noexcept {
    if (!std::isfinite(value)) {
        return VN_INVALID_ARG;
    }
    Variable* variable = FindVariable(id);
    if (variable == nullptr) {
        if (m_variableCount >= kMaxVariables) {
            return VN_NO_VARIABLE;
        }
        variable = &m_variables[m_variableCount++];
        variable->id = id;
        variable->value = value;
        NoteVariableWrite(id);
        return VN_OK;
    }
    if (variable->value != value) {
        variable->value = value;
        NoteVariableWrite(id);
    }
    return VN_OK;
}

void ParamSystem::ClearInstalledValues() noexcept {
    for (std::uint32_t i = 0; i < m_variableCount; ++i) {
        m_variables[i] = Variable{};
    }
    m_variableCount = 0;
}

VnResult ParamSystem::PrepareVariable(
    VnVariableId id, float value, std::uint32_t slotIndex,
    PreparedVariableToken* out) const {
    if (out == nullptr || out->m_prepared || !std::isfinite(value)) {
        return VN_INVALID_ARG;
    }
    if (const Variable* existing = FindVariable(id)) {
        out->m_id = id;
        out->m_value = value;
        out->m_slotIndex =
            static_cast<std::uint32_t>(existing - m_variables.data());
        out->m_append = false;
        out->m_changes = existing->value != value;
        out->m_prepared = true;
        return VN_OK;
    }
    if (slotIndex < m_variableCount || slotIndex >= kMaxVariables) {
        return VN_NO_VARIABLE;
    }
    out->m_id = id;
    out->m_value = value;
    out->m_slotIndex = slotIndex;
    out->m_append = true;
    out->m_changes = true;
    out->m_prepared = true;
    return VN_OK;
}

void ParamSystem::CancelPreparedVariable(
    PreparedVariableToken* token) noexcept {
    if (token != nullptr) {
        *token = PreparedVariableToken{};
    }
}

void ParamSystem::CommitPreparedVariable(
    PreparedVariableToken* token) noexcept {
    assert(token != nullptr && token->m_prepared);
    assert(token->m_slotIndex < m_variables.size());
    if (token->m_append) {
        assert(token->m_slotIndex == m_variableCount);
    } else {
        assert(token->m_slotIndex < m_variableCount);
        assert(m_variables[token->m_slotIndex].id == token->m_id);
    }
    Variable& variable = m_variables[token->m_slotIndex];
    const bool changed = token->m_changes;
    variable.id = token->m_id;
    variable.value = token->m_value;
    if (token->m_append) {
        ++m_variableCount;
    }
    if (changed) {
        NoteVariableWrite(token->m_id);
    }
    *token = PreparedVariableToken{};
}

void ParamSystem::NoteVariableWrite(VnVariableId id) noexcept {
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        Binding& binding = m_bindings[i];
        if (binding.variableId != id) {
            continue;
        }
        VoiceState* state = StateFor(binding.handle);
        if (state == nullptr) {
            continue; // dead voice: the binding is inert
        }
        // Over-marking is deliberate (a global write may be shadowed by an
        // emitter-local override): evaluation realizes real changes as
        // dirty rebuilt targets and retires the rest, publishing nothing
        // for them. Under-reserving would be the real defect.
        SetTargetPendingInput(state->targets[binding.targetId]);
    }
}

void ParamSystem::NoteEmitterDetached(VnEmitterHandle emitter) {
    if (emitter == 0) {
        return;
    }
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        Binding& binding = m_bindings[i];
        VoiceState* state = StateFor(binding.handle);
        if (state == nullptr || state->emitterHandle != emitter) {
            continue;
        }
        // The voice's scoped reads fall back to global from now on; the
        // next evaluation realizes whichever contributions that moves.
        SetTargetPendingInput(state->targets[binding.targetId]);
    }
    for (VoiceState& state : m_voiceStates) {
        if (!state.retired && state.emitterHandle == emitter) {
            state.emitterHandle = 0;
        }
    }
}

ParamSystem::InstanceBindResult
ParamSystem::AdmitBindingSlot(VnVoiceHandle handle, std::uint32_t targetId) {
    // The per-voice ceiling counts every occupied slot (raw bindings and
    // instances, neutralized included — slots free only at voice end).
    if (VoiceBindingCount(handle) >= m_instanceSlotsPerVoice) {
        return InstanceBindResult::SlotCeiling;
    }
    if (m_bindingCount >= m_bindings.size()) {
        return InstanceBindResult::TableFull;
    }
    // Section 10.4: a NEW unique (voice, target) pair on a LIVE voice adds
    // one commit to the worst-case one-tick publication; growth past the
    // command queue's capacity refuses here, before any state changes —
    // the same refusal shape as a full table. Duplicate pairs add no
    // demand (one target publishes at most one command per tick), and a
    // binding on a voice that never started is inert forever (handles are
    // generational), so it adds none either.
    if (m_capacityPublisher != nullptr && StateFor(handle) != nullptr &&
        !HasBinding(handle, targetId) &&
        !m_capacityPublisher->TryGrowWorstCase(1, m_capacityLog)) {
        return InstanceBindResult::CapacityRefused;
    }
    return InstanceBindResult::Ok;
}

bool ParamSystem::BindVoiceTarget(VnVariableId variableId,
                                  VnVoiceHandle handle,
                                  std::uint32_t targetId,
                                  const Curve& curve) {
    if (targetId >= kBuiltinTargetCount || curve.pointCount == 0) {
        return false;
    }
    if (AdmitBindingSlot(handle, targetId) != InstanceBindResult::Ok) {
        return false;
    }
    Binding& binding = m_bindings[m_bindingCount++];
    if (m_bindingCount > m_instanceHighWater) {
        m_instanceHighWater = m_bindingCount;
    }
    // Reset the reused entry wholesale: removal compaction recycles
    // storage, and a stale definition field would corrupt the new entry.
    binding = Binding{};
    binding.variableId = variableId;
    binding.handle = handle;
    binding.targetId = targetId;
    binding.curve = curve;
    // everEvaluated starts false, so the next flush treats this binding as a
    // change and evaluates it — whether the Variable already has a value
    // (global or emitter-local) or is first set later. On a live voice
    // that first evaluation is known future demand: mark it now so an
    // active deferral's reservation covers it before any later admission.
    if (VoiceState* state = StateFor(handle)) {
        SetTargetPendingInput(state->targets[targetId]);
    }
    return true;
}

ParamSystem::InstanceBindResult ParamSystem::BindVoiceInstance(
    const BindingDefinitionView& view, VnVoiceHandle handle,
    VnObjectId attachmentObjectId, bool viaApply) {
    // Instance identity is the full (definition, attachment object,
    // viaApply) triple: only an exact-identity duplicate refuses. One
    // definition attached to both the played id and the resolved leaf
    // legitimately instantiates twice and contributes twice (the
    // attachment layer's fan-out, section 5.3); a repeat of the SAME
    // attachment — or a second apply — would double one authoring fact
    // and refuses. Neutralized instances do not block a fresh one
    // (their contribution is gone; only their slot lingers).
    if (view.snapshotOwned &&
        (view.curve == nullptr || view.curve->pointCount == 0)) {
        return InstanceBindResult::InvalidCurve;
    }
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        const Binding& b = m_bindings[i];
        if (b.handle == handle && b.definitionId == view.definitionId &&
            b.attachmentObjectId == attachmentObjectId &&
            b.viaApply == viaApply && !b.neutralized) {
            return InstanceBindResult::Duplicate;
        }
    }
    const InstanceBindResult admitted =
        AdmitBindingSlot(handle, view.targetId);
    if (admitted != InstanceBindResult::Ok) {
        return admitted;
    }
    Binding& binding = m_bindings[m_bindingCount++];
    if (m_bindingCount > m_instanceHighWater) {
        m_instanceHighWater = m_bindingCount;
    }
    binding = Binding{};
    binding.variableId = view.variableId;
    binding.handle = handle;
    binding.targetId = view.targetId;
    binding.definitionId = view.definitionId;
    binding.attachmentObjectId = attachmentObjectId;
    binding.viaApply = viaApply;
    binding.snapshotOwned = view.snapshotOwned;
    if (view.snapshotOwned) {
        binding.curve = *view.curve;
        binding.ownedTransitionMs =
            view.transitionMs != nullptr ? *view.transitionMs : 0u;
        binding.lastRevision =
            view.revision != nullptr ? *view.revision : 0u;
        binding.liveCurve = nullptr;
        binding.liveTransitionMs = nullptr;
        binding.liveRevision = nullptr;
    } else {
        binding.liveCurve = view.curve;
        binding.liveTransitionMs = view.transitionMs;
        binding.liveRevision = view.revision;
        binding.lastRevision =
            view.revision != nullptr ? *view.revision : 0u;
    }
    if (VoiceState* state = StateFor(handle)) {
        SetTargetPendingInput(state->targets[binding.targetId]);
    }
    return InstanceBindResult::Ok;
}

ParamSystem::InstanceBindResult ParamSystem::CanBindVoiceInstance(
    const BindingDefinitionView& view, VnVoiceHandle handle,
    VnObjectId attachmentObjectId, bool viaApply) const {
    if (view.snapshotOwned &&
        (view.curve == nullptr || view.curve->pointCount == 0)) {
        return InstanceBindResult::InvalidCurve;
    }
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        const Binding& binding = m_bindings[i];
        if (binding.handle == handle &&
            binding.definitionId == view.definitionId &&
            binding.attachmentObjectId == attachmentObjectId &&
            binding.viaApply == viaApply && !binding.neutralized) {
            return InstanceBindResult::Duplicate;
        }
    }
    if (VoiceBindingCount(handle) >= m_instanceSlotsPerVoice) {
        return InstanceBindResult::SlotCeiling;
    }
    if (m_bindingCount >= m_bindings.size()) {
        return InstanceBindResult::TableFull;
    }
    if (m_capacityPublisher != nullptr && StateFor(handle) != nullptr &&
        !HasBinding(handle, view.targetId) &&
        m_capacityPublisher->WorstCaseCommits() >=
            m_capacityPublisher->BatchCapacity()) {
        return InstanceBindResult::CapacityRefused;
    }
    return InstanceBindResult::Ok;
}

std::uint32_t
ParamSystem::NeutralizeInstancesOf(VnObjectId definitionId,
                                   VnObjectId attachmentObjectId) {
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        Binding& b = m_bindings[i];
        if (b.definitionId != definitionId || b.viaApply ||
            b.attachmentObjectId != attachmentObjectId || b.neutralized) {
            continue;
        }
        NeutralizeEntry(b);
        ++count;
    }
    return count;
}

bool ParamSystem::NeutralizeVoiceInstance(VnVoiceHandle handle,
                                          VnObjectId definitionId) {
    // The clear verb is the apply verb's exact inverse: it targets the
    // apply-born identity (definitionId, object 0, viaApply) only.
    // Attachment-born instances of the same definition are a different
    // identity and belong to detach.
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        Binding& b = m_bindings[i];
        if (b.handle == handle && b.definitionId == definitionId &&
            b.viaApply && !b.neutralized) {
            NeutralizeEntry(b);
            return true;
        }
    }
    return false;
}

void ParamSystem::NeutralizeEntry(Binding& binding) {
    binding.neutralized = true;
    // One exit evaluation realizes the contribution's departure: the next
    // gather rebuilds the target without this instance and publishes the
    // change ramped with the definition's transitionMs toward neutral
    // (section 9.2's detach rule).
    binding.neutralizePending = true;
    if (VoiceState* state = StateFor(binding.handle)) {
        TargetState& target = state->targets[binding.targetId];
        SetTargetPendingInput(target);
        const std::uint32_t transition = BindingTransitionMs(binding);
        if (!target.suppressClaimRamp &&
            transition > target.pendingRampMs) {
            target.pendingRampMs = transition;
        }
    }
}

void ParamSystem::PurgeInstancesOf(VnObjectId definitionId) {
    // Remember the removed entries' (voice, target) pairs — deduplicated,
    // because two identities of one definition may share a pair (an
    // attachment-born and an apply-born instance, or a neutralized
    // leftover beside its live re-apply) while the section-10.4 growth
    // counted the pair ONCE — plus whether any removed entry still owed
    // the slot a contribution (live, or a neutralize whose exit
    // evaluation has not run yet).
    std::vector<std::pair<VnVoiceHandle, std::uint32_t>> removed;
    std::vector<bool> owedExit;
    std::uint32_t write = 0;
    for (std::uint32_t read = 0; read < m_bindingCount; ++read) {
        Binding& b = m_bindings[read];
        if (b.definitionId == definitionId) {
            const bool owes = !b.neutralized || b.neutralizePending;
            bool seen = false;
            for (std::size_t k = 0; k < removed.size(); ++k) {
                if (removed[k].first == b.handle &&
                    removed[k].second == b.targetId) {
                    seen = true;
                    if (owes) {
                        owedExit[k] = true;
                    }
                    break;
                }
            }
            if (!seen) {
                removed.emplace_back(b.handle, b.targetId);
                owedExit.push_back(owes);
            }
            continue;
        }
        if (write != read) {
            m_bindings[write] = m_bindings[read];
        }
        ++write;
    }
    m_bindingCount = write;
    for (std::size_t k = 0; k < removed.size(); ++k) {
        VoiceState* state = StateFor(removed[k].first);
        if (state == nullptr) {
            continue; // slot recycled: demand released at reuse, no slot
        }
        // A departed feeder that still owed a contribution exit forces
        // its target's slot to rebuild from the survivors at the next
        // evaluation and publishes the exit (the neutralize-then-
        // unregister-same-tick sequence must still ramp the contribution
        // to neutral, section 5.3); the exit ramp was folded into
        // pendingRampMs at neutralize time and rides that publication.
        if (owedExit[k]) {
            TargetState& target = state->targets[removed[k].second];
            if (!target.rebuild) {
                target.rebuild = true;
                target.active[kSlotVariableBindings] = false;
                target.slots[kSlotVariableBindings] = 0.0f;
                ++m_forcedRebuilds;
            }
            SetTargetDirty(*state, target);
        }
        // Only the last feeder of a still-tracked voice's target
        // releases worst-case demand — once per unique pair, matching
        // the one grow the pair's first bind admitted.
        if (m_capacityPublisher != nullptr &&
            !HasBinding(removed[k].first, removed[k].second)) {
            m_capacityPublisher->ShrinkWorstCase(1);
        }
    }
}

VnResult ParamSystem::PreparePurgeInstancesOf(
    const VnObjectId* definitionIds,
    std::uint32_t definitionCount, PreparedPurgeExit* scratch,
    std::uint32_t scratchCapacity,
    PreparedPurgeToken* out) const noexcept {
    if (out == nullptr ||
        (definitionCount != 0 && definitionIds == nullptr)) {
        return VN_INVALID_ARG;
    }
    *out = PreparedPurgeToken{};
    const auto selected = [&](VnObjectId id) noexcept {
        for (std::uint32_t i = 0; i < definitionCount; ++i) {
            if (definitionIds[i] == id) {
                return true;
            }
        }
        return false;
    };
    std::uint32_t exitCount = 0;
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        const Binding& binding = m_bindings[i];
        if (!selected(binding.definitionId)) {
            continue;
        }
        const bool owes =
            !binding.neutralized || binding.neutralizePending;
        bool seen = false;
        for (std::uint32_t exit = 0; exit < exitCount; ++exit) {
            if (scratch[exit].handle == binding.handle &&
                scratch[exit].targetId == binding.targetId) {
                scratch[exit].owedExit |= owes;
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        if (exitCount >= scratchCapacity || scratch == nullptr) {
            return VN_NO_BINDING;
        }
        scratch[exitCount++] = PreparedPurgeExit{
            binding.handle, binding.targetId, owes};
    }
    out->m_definitionIds = definitionIds;
    out->m_definitionCount = definitionCount;
    out->m_exits = scratch;
    out->m_exitCount = exitCount;
    out->m_prepared = true;
    return VN_OK;
}

void ParamSystem::CancelPreparedPurgeInstances(
    PreparedPurgeToken* token) noexcept {
    if (token != nullptr) {
        *token = PreparedPurgeToken{};
    }
}

void ParamSystem::CommitPreparedPurgeInstances(
    PreparedPurgeToken* token) noexcept {
    static_assert(std::is_nothrow_copy_assignable_v<Binding>);
    static_assert(std::is_nothrow_copy_assignable_v<PreparedPurgeToken>);
    if (token == nullptr || !token->IsPrepared()) {
        return;
    }
    const auto selected = [&](VnObjectId id) noexcept {
        for (std::uint32_t i = 0; i < token->m_definitionCount; ++i) {
            if (token->m_definitionIds[i] == id) {
                return true;
            }
        }
        return false;
    };
    std::uint32_t write = 0;
    for (std::uint32_t read = 0; read < m_bindingCount; ++read) {
        if (selected(m_bindings[read].definitionId)) {
            continue;
        }
        if (write != read) {
            m_bindings[write] = m_bindings[read];
        }
        ++write;
    }
    m_bindingCount = write;
    for (std::uint32_t i = 0; i < token->m_exitCount; ++i) {
        const PreparedPurgeExit& exit = token->m_exits[i];
        VoiceState* state = StateFor(exit.handle);
        if (state == nullptr) {
            continue;
        }
        if (exit.owedExit) {
            TargetState& target = state->targets[exit.targetId];
            if (!target.rebuild) {
                target.rebuild = true;
                target.active[kSlotVariableBindings] = false;
                target.slots[kSlotVariableBindings] = 0.0f;
                ++m_forcedRebuilds;
            }
            SetTargetDirty(*state, target);
        }
        if (m_capacityPublisher != nullptr &&
            !HasBinding(exit.handle, exit.targetId)) {
            m_capacityPublisher->ShrinkWorstCase(1);
        }
    }
    *token = PreparedPurgeToken{};
}

std::uint32_t ParamSystem::PurgeVoiceInstances(VnVoiceHandle handle, bool snapshotOnly) {
    // Removes the voice's definition-backed entries WITHOUT releasing
    // worst-case demand: the caller (the park path) takes ownership of
    // the returned unique-pair count as a held reservation, so the
    // parked logical voice keeps its section-10.4 seat and the return's
    // re-binds grow back into exactly the room this hold guarantees.
    bool removed[kBuiltinTargetCount]{};
    std::uint32_t write = 0;
    for (std::uint32_t read = 0; read < m_bindingCount; ++read) {
        Binding& b = m_bindings[read];
        if (b.handle == handle && b.definitionId != VnObjectId{} && (!snapshotOnly || b.snapshotOwned)) {
            removed[b.targetId] = true;
            continue;
        }
        if (write != read) {
            m_bindings[write] = m_bindings[read];
        }
        ++write;
    }
    m_bindingCount = write;
    if (SlotOf(handle) >= m_voiceStates.size() || m_voiceStates[SlotOf(handle)].handle != handle) {
        return 0; // never-started handle: its binds never grew demand
    }
    // Pairs a surviving raw binding of the same voice still feeds keep
    // their demand with the raw side (growth was deduplicated across
    // both kinds), so only the instance-exclusive pairs transfer.
    std::uint32_t transferred = 0;
    for (std::uint32_t targetId = 0; targetId < kBuiltinTargetCount; ++targetId) {
        if (removed[targetId] && !HasBinding(handle, targetId)) {
            ++transferred;
        }
    }
    return transferred;
}

void ParamSystem::NoteDefinitionReplaced(VnObjectId definitionId) {
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        Binding& b = m_bindings[i];
        if (b.definitionId != definitionId || b.neutralized) {
            continue;
        }
        VoiceState* state = StateFor(b.handle);
        if (state == nullptr) {
            continue; // dead voice: the instance is inert
        }
        // The revision mismatch makes the next evaluation re-evaluate the
        // instance; this mark grows the live demand census now, exactly
        // like a Variable write (an active deferral's reservation must
        // cover the install's future publications).
        SetTargetPendingInput(state->targets[b.targetId]);
    }
}

bool ParamSystem::RefreshPreviewBindings(VnVoiceHandle handle,
    const BindingTable& definitions, bool allowPitch) {
    VoiceState* state = StateFor(handle);
    if (state == nullptr) return false;
    bool complete = true;
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        Binding& binding = m_bindings[i];
        if (binding.handle != handle || !binding.snapshotOwned || binding.neutralized)
            continue;
        const auto* definition = definitions.Find(binding.definitionId);
        if (definition == nullptr || definition->scopeKind != kTargetScopeVoice
            || definition->variableId != binding.variableId
            || definition->targetId != binding.targetId
            || definition->revision == binding.previewRevision)
            continue;
        if (ForeignClaim(handle, binding.targetId)
            || (binding.targetId == kTargetVoicePitch && !allowPitch)) {
            complete = false;
            continue;
        }
        binding.curve = definition->curve;
        binding.ownedTransitionMs = definition->transitionMs;
        binding.previewRevision = definition->revision;
        binding.lastRevision = definition->revision;
        binding.definitionChanged = true;
        SetTargetPendingInput(state->targets[binding.targetId]);
    }
    return complete;
}

std::uint32_t ParamSystem::VoiceBindingCount(VnVoiceHandle handle) const {
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        if (m_bindings[i].handle == handle) {
            ++count;
        }
    }
    return count;
}

float ParamSystem::EvaluateBinding(const Binding& binding, float input) const {
    const Curve& curve = binding.liveCurve != nullptr ? *binding.liveCurve : binding.curve;
    return curve.Eval(input, BindingCurveDomain(kTargetScopeVoice, binding.targetId));
}

float ParamSystem::InstanceContribution(VnVoiceHandle handle,
                                        std::uint32_t targetId,
                                        const EmitterTable* emitters) const {
    const VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return 0.0f;
    }
    float sum = 0.0f;
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        const Binding& b = m_bindings[i];
        if (b.handle != handle || b.definitionId == VnObjectId{} || b.neutralized ||
            b.targetId != targetId) {
            continue;
        }
        float input = 0.0f;
        if (ResolveScopedValue(emitters, state->emitterHandle, b.variableId,
                               &input)) {
            sum += EvaluateBinding(b, input);
        }
    }
    return sum;
}

bool ParamSystem::TryPeekResolvedVolume(VnVoiceHandle handle,
                                       const EmitterTable* emitters,
                                       float* outDb) const {
    if (outDb == nullptr) {
        return false;
    }
    const VoiceState* state = StateFor(handle);
    if (state == nullptr) {
        return false;
    }
    bool fed = false;
    float slot = 0.0f;
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        const Binding& binding = m_bindings[i];
        if (binding.handle != handle ||
            binding.targetId != kTargetVoiceVolume) {
            continue;
        }
        float input = 0.0f;
        const bool hasValue = ResolveScopedValue(
            emitters, state->emitterHandle, binding.variableId, &input);
        if (binding.neutralized || !hasValue) {
            continue;
        }
        const float contribution = EvaluateBinding(binding, input);
        if (!fed) {
            slot = contribution;
            fed = true;
        } else {
            slot += contribution;
        }
    }
    *outDb = fed ? Reduce(*state, kTargetVoiceVolume, kSlotVariableBindings,
                          slot)
                 : Reduce(*state, kTargetVoiceVolume);
    return true;
}

bool ParamSystem::TryClaim(VnVoiceHandle handle, std::uint32_t targetId,
                           VariableSliceRef owner,
                           std::uint32_t expectedRevision) noexcept {
    VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount || !owner.Valid()) {
        return false;
    }
    TargetState& target = state->targets[targetId];
    if (target.revision != expectedRevision || target.preparedOwner.Valid()) {
        return false;
    }
    target.preparedOwner = owner;
    target.preparedRampMs = 0;
    target.suppressClaimRamp = false;
    target.claimSnapshotCaptured = false;
    target.preCandidatePendingRampMs = 0;
    target.preCandidateApiRampMs = 0;
    target.preCandidateApiRampTick = 0;
    target.candidateBaselineValid = false;
    return true;
}

bool ParamSystem::CaptureCandidateBaseline(VnVoiceHandle handle,
                                           std::uint32_t targetId,
                                           VariableSliceRef owner) noexcept {
    VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount || !owner.Valid()) {
        return false;
    }
    TargetState& target = state->targets[targetId];
    if (target.preparedOwner.slot != owner.slot ||
        target.preparedOwner.generation != owner.generation ||
        target.preparedRampMs != 0 || target.claimSnapshotCaptured) {
        return false;
    }
    if (!target.candidateBaselineValid) {
        target.preCandidatePendingRampMs = target.pendingRampMs;
        target.preCandidateApiRampMs = target.apiRampMs;
        target.preCandidateApiRampTick = target.apiRampTick;
        target.candidateBaselineValid = true;
    }
    return true;
}

void ParamSystem::ClearCandidateBaseline(VnVoiceHandle handle,
                                         std::uint32_t targetId,
                                         VariableSliceRef owner) noexcept {
    VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return;
    }
    TargetState& target = state->targets[targetId];
    if (target.preparedOwner.slot != owner.slot ||
        target.preparedOwner.generation != owner.generation) {
        return;
    }
    target.preCandidatePendingRampMs = 0;
    target.preCandidateApiRampMs = 0;
    target.preCandidateApiRampTick = 0;
    target.candidateBaselineValid = false;
}

void ParamSystem::DropClaimedPublication(VnVoiceHandle handle,
                                         std::uint32_t targetId,
                                         VariableSliceRef owner) noexcept {
    VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return;
    }
    TargetState& target = state->targets[targetId];
    if (target.preparedOwner.slot != owner.slot ||
        target.preparedOwner.generation != owner.generation) {
        return;
    }
    RecordPublishedValue(*state, targetId, Reduce(*state, targetId), Pan2dOf(*state));
    SetTargetClean(target);
    target.pendingRampMs = 0;
    target.preparedRampMs = 0;
    target.suppressClaimRamp = false;
    target.claimSnapshotCaptured = false;
    target.preCandidatePendingRampMs = 0;
    target.preCandidateApiRampMs = 0;
    target.preCandidateApiRampTick = 0;
    target.candidateBaselineValid = false;
    target.preparedOwner = {};
}

void ParamSystem::ReleaseClaim(VnVoiceHandle handle, std::uint32_t targetId,
                               VariableSliceRef owner,
                               ClaimReleaseReason reason) noexcept {
    VoiceState* state = StateFor(handle);
    if (state != nullptr && targetId < kBuiltinTargetCount) {
        TargetState& target = state->targets[targetId];
        if (target.preparedOwner.slot == owner.slot &&
            target.preparedOwner.generation == owner.generation) {
            if (reason == ClaimReleaseReason::RejectCandidate) {
                if (target.candidateBaselineValid) {
                    target.pendingRampMs = target.preCandidatePendingRampMs;
                    target.apiRampMs = target.preCandidateApiRampMs;
                    target.apiRampTick = target.preCandidateApiRampTick;
                }
            } else {
                target.pendingRampMs =
                    std::max(target.pendingRampMs, target.preparedRampMs);
            }
            target.preparedRampMs = 0;
            target.suppressClaimRamp = false;
            target.claimSnapshotCaptured = false;
            target.preCandidatePendingRampMs = 0;
            target.preCandidateApiRampMs = 0;
            target.preCandidateApiRampTick = 0;
            target.candidateBaselineValid = false;
            target.preparedOwner = {};
            return;
        }
    }
    if (RetiringTarget* retiring =
            FindRetiringTarget(handle, targetId, owner)) {
        *retiring = RetiringTarget{};
    }
}

bool ParamSystem::SnapshotForClaim(VnVoiceHandle handle,
                                   std::uint32_t targetId,
                                   VariableSliceRef owner, float* resolved,
                                   std::uint32_t* rampMs,
                                   std::uint32_t* revision, float* pan2d) const noexcept {
    const VoiceState* state = StateFor(handle);
    if (resolved == nullptr || rampMs == nullptr || revision == nullptr) {
        return false;
    }
    if (state != nullptr && targetId < kBuiltinTargetCount) {
        const TargetState& target = state->targets[targetId];
        if (target.preparedOwner.slot == owner.slot &&
            target.preparedOwner.generation == owner.generation) {
            *resolved = Reduce(*state, targetId);
            if (pan2d != nullptr) *pan2d = targetId == kTargetVoicePan ? Pan2dOf(*state) : 0.0f;
            *rampMs = target.pendingRampMs;
            *revision = target.revision;
            return true;
        }
    }
    const RetiringTarget* retiring =
        FindRetiringTarget(handle, targetId, owner);
    if (retiring == nullptr) {
        return false;
    }
    *resolved = retiring->frozenResolved;
    if (pan2d != nullptr) *pan2d = retiring->frozenPan2d;
    *rampMs = retiring->frozenRampMs;
    *revision = retiring->revision;
    return true;
}

bool ParamSystem::CaptureSnapshotForClaim(
    VnVoiceHandle handle, std::uint32_t targetId, VariableSliceRef owner,
    float* resolved, std::uint32_t* rampMs,
    std::uint32_t* revision, float* pan2d) noexcept {
    if (resolved == nullptr || rampMs == nullptr || revision == nullptr) {
        return false;
    }
    VoiceState* state = StateFor(handle);
    if (state != nullptr && targetId < kBuiltinTargetCount) {
        TargetState& target = state->targets[targetId];
        if (target.preparedOwner.slot == owner.slot &&
            target.preparedOwner.generation == owner.generation) {
            if (target.claimSnapshotCaptured) {
                return false;
            }
            *resolved = Reduce(*state, targetId);
            if (pan2d != nullptr) *pan2d = targetId == kTargetVoicePan ? Pan2dOf(*state) : 0.0f;
            *rampMs = target.pendingRampMs;
            *revision = target.revision;
            target.preparedRampMs = target.pendingRampMs;
            target.pendingRampMs = 0;
            target.claimSnapshotCaptured = true;
            return true;
        }
    }
    RetiringTarget* retiring = FindRetiringTarget(handle, targetId, owner);
    if (retiring == nullptr || retiring->claimSnapshotCaptured) {
        return false;
    }
    *resolved = retiring->frozenResolved;
    if (pan2d != nullptr) *pan2d = retiring->frozenPan2d;
    *rampMs = retiring->frozenRampMs;
    *revision = retiring->revision;
    retiring->claimSnapshotCaptured = true;
    return true;
}

void ParamSystem::SetClaimRampSuppressed(
    VnVoiceHandle handle, std::uint32_t targetId, VariableSliceRef owner,
    bool suppressed) noexcept {
    VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return;
    }
    TargetState& target = state->targets[targetId];
    if (target.preparedOwner.slot == owner.slot &&
        target.preparedOwner.generation == owner.generation) {
        target.suppressClaimRamp = suppressed;
    }
}

std::uint32_t ParamSystem::TargetRevision(VnVoiceHandle handle,
                                          std::uint32_t targetId) const noexcept {
    const VoiceState* state = StateFor(handle);
    if (state != nullptr && targetId < kBuiltinTargetCount) {
        return state->targets[targetId].revision;
    }
    for (const RetiringTarget& retiring : m_retiringTargets) {
        if (retiring.used && retiring.handle == handle &&
            retiring.targetId == targetId) {
            return retiring.revision;
        }
    }
    return 0;
}

bool ParamSystem::ForeignClaim(VnVoiceHandle handle,
                               std::uint32_t targetId) const noexcept {
    const VoiceState* state = StateFor(handle);
    if (state != nullptr && targetId < kBuiltinTargetCount &&
        state->targets[targetId].preparedOwner.Valid()) {
        return true;
    }
    for (const RetiringTarget& retiring : m_retiringTargets) {
        if (retiring.used && retiring.handle == handle &&
            retiring.targetId == targetId) {
            return true;
        }
    }
    return false;
}

void ParamSystem::MarkSliceCommitted(VnVoiceHandle handle,
                                     std::uint32_t targetId,
                                     VariableSliceRef owner, float resolved,
                                     std::uint32_t rampMs, float pan2d) noexcept {
    VoiceState* state = StateFor(handle);
    if (state != nullptr && targetId < kBuiltinTargetCount) {
        TargetState& target = state->targets[targetId];
        if (target.preparedOwner.slot == owner.slot &&
            target.preparedOwner.generation == owner.generation) {
            RecordPublishedValue(*state, targetId, resolved, pan2d);
            target.lastPublishedRampMs = rampMs;
            if (PublicationChanged(*state, targetId, Reduce(*state, targetId),
                                   Pan2dOf(*state))) {
                SetTargetDirty(*state, target);
            } else {
                target.pendingRampMs = 0;
                SetTargetClean(target);
            }
            target.preparedRampMs = 0;
            target.suppressClaimRamp = false;
            target.claimSnapshotCaptured = false;
            target.preCandidatePendingRampMs = 0;
            target.preCandidateApiRampMs = 0;
            target.preCandidateApiRampTick = 0;
            target.candidateBaselineValid = false;
            target.preparedOwner = {};
            ++m_commits;
            return;
        }
    }
    if (RetiringTarget* retiring =
            FindRetiringTarget(handle, targetId, owner)) {
        *retiring = RetiringTarget{};
    }
}

std::uint32_t ParamSystem::RetiringTargetCountForTests() const noexcept {
    std::uint32_t count = 0;
    for (const RetiringTarget& target : m_retiringTargets) {
        count += target.used ? 1u : 0u;
    }
    return count;
}

float ParamSystem::ResolvedValue(VnVoiceHandle handle,
                                 std::uint32_t targetId) const {
    const VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return 0.0f;
    }
    return Reduce(*state, targetId);
}

float ParamSystem::ReduceWithSlot(VnVoiceHandle handle,
                                  std::uint32_t targetId,
                                  std::uint32_t slot,
                                  float domainValue) const {
    const VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount ||
        slot >= kCombineSlotCount) {
        return 0.0f;
    }
    return Reduce(*state, targetId, slot, domainValue);
}

std::uint32_t ParamSystem::PendingRampMs(VnVoiceHandle handle,
                                         std::uint32_t targetId) const {
    const VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return 0;
    }
    const TargetState& target = state->targets[targetId];
    std::uint32_t rampMs = target.pendingRampMs;
    if (target.apiRampTick == m_flushTick && target.apiRampMs > rampMs) {
        rampMs = target.apiRampMs;
    }
    return rampMs;
}

void ParamSystem::CaptureVoiceState(
    VnVoiceHandle handle, std::vector<ApiDirectState>* apiDirect,
    std::vector<BindingState>* bindings) const {
    apiDirect->clear();
    bindings->clear();
    const VoiceState* state = StateFor(handle);
    if (state != nullptr) {
        for (std::uint32_t t = 0; t < kBuiltinTargetCount; ++t) {
            const TargetState& target = state->targets[t];
            if (target.active[kSlotApiDirect]) {
                ApiDirectState a;
                a.targetId = t;
                a.value = target.slots[kSlotApiDirect];
                apiDirect->push_back(a);
            }
        }
    }
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        const Binding& b = m_bindings[i];
        // Raw bindings only: definition-backed instances are captured as
        // identities (CaptureVoiceInstances), never as curve copies.
        if (b.handle == handle && b.definitionId == VnObjectId{}) {
            BindingState s;
            s.variableId = b.variableId;
            s.targetId = b.targetId;
            s.curve = b.curve;
            bindings->push_back(s);
        }
    }
}

void ParamSystem::CaptureVoiceInstances(
    VnVoiceHandle handle, std::vector<InstanceState>* instances) const {
    instances->clear();
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        const Binding& b = m_bindings[i];
        // Neutralized instances stay behind: their contribution is gone
        // for good, so a return has nothing to re-apply for them.
        if (b.handle != handle || b.definitionId == VnObjectId{} || b.neutralized) {
            continue;
        }
        InstanceState s;
        s.definitionId = b.definitionId;
        s.attachmentObjectId = b.attachmentObjectId;
        s.viaApply = b.viaApply;
        instances->push_back(s);
    }
}

std::uint32_t ParamSystem::PrimeVoiceBindings(VnVoiceHandle handle,
                                              const EmitterTable* emitters,
                                              PrimedValue* out,
                                              std::uint32_t capacity) {
    VoiceState* state = StateFor(handle);
    if (state == nullptr) {
        return 0;
    }
    // Reset the Variable-bindings slot of every target this voice's
    // bindings feed, then accumulate every contributor at its current
    // scoped input and current definition revision — the same two-pass
    // shape as the ordinary evaluation, restricted to one voice.
    bool fed[kBuiltinTargetCount] = {};
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        const Binding& b = m_bindings[i];
        if (b.handle != handle || b.targetId >= kBuiltinTargetCount) {
            continue;
        }
        if (!fed[b.targetId]) {
            fed[b.targetId] = true;
            TargetState& target = state->targets[b.targetId];
            target.active[kSlotVariableBindings] = false;
            target.slots[kSlotVariableBindings] = 0.0f;
        }
    }
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        Binding& b = m_bindings[i];
        if (b.handle != handle || b.targetId >= kBuiltinTargetCount) {
            continue;
        }
        float input = 0.0f;
        const bool hasValue = ResolveScopedValue(
            emitters, state->emitterHandle, b.variableId, &input);
        // Recording the change-detection state here is what makes the
        // tick's ordinary evaluation see no spurious change (and fold no
        // activation ramp): the voice's first values snap by publishing
        // through the caller's start batch, never through the flush.
        b.lastInput = input;
        b.lastHadValue = hasValue;
        b.everEvaluated = true;
        if (b.liveRevision != nullptr) {
            b.lastRevision = *b.liveRevision;
        }
        b.neutralizePending = false;
        if (b.neutralized || !hasValue) {
            b.lastContribution = 0.0f;
            continue;
        }
        const float contribution = EvaluateBinding(b, input);
        b.lastContribution = contribution;
        TargetState& target = state->targets[b.targetId];
        float& slot = target.slots[kSlotVariableBindings];
        if (!target.active[kSlotVariableBindings]) {
            slot = contribution;
            target.active[kSlotVariableBindings] = true;
            continue;
        }
        switch (DomainOfTarget(b.targetId)) {
        case CombineDomain::AddDb:
        case CombineDomain::AddCents:
        case CombineDomain::Add:
        case CombineDomain::AddLogHz:
        case CombineDomain::AddLogQ:
            slot += contribution;
            break;
        case CombineDomain::MinHz:
            slot = (contribution < slot) ? contribution : slot;
            break;
        case CombineDomain::MaxHz:
        case CombineDomain::Max:
            slot = (contribution > slot) ? contribution : slot;
            break;
        }
    }
    std::uint32_t written = 0;
    for (std::uint32_t t = 0; t < kBuiltinTargetCount; ++t) {
        if (!fed[t]) {
            continue;
        }
        TargetState& target = state->targets[t];
        const float resolved = Reduce(*state, t);
        const float pan2d = t == kTargetVoicePan ? Pan2dOf(*state) : 0.0f;
        if (PublicationChanged(*state, t, resolved, pan2d)) {
            if (written >= capacity) {
                // Defensive only (callers size to kBuiltinTargetCount):
                // an unwritten value stays dirty for the ordinary flush
                // rather than being silently lost.
                SetTargetDirty(*state, target);
                continue;
            }
            out[written].targetId = t;
            out[written].resolved = resolved;
            out[written].pan2d = pan2d;
            ++written;
            RecordPublishedValue(*state, t, resolved, pan2d);
        }
        // The primed target is settled: its demand marks retire and no
        // ramp is pending (initial instantiation snaps, section 9.2).
        ClearTargetPendingInput(target);
        SetTargetClean(target);
        target.pendingRampMs = 0;
    }
    return written;
}

std::uint32_t ParamSystem::PrimeTargets(VnVoiceHandle handle,
                                        const std::uint32_t* targetIds,
                                        std::uint32_t count,
                                        PrimedValue* out,
                                        std::uint32_t capacity) {
    VoiceState* state = StateFor(handle);
    if (state == nullptr) {
        return 0;
    }
    std::uint32_t written = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t t = targetIds[i];
        if (t >= kBuiltinTargetCount) {
            continue;
        }
        TargetState& target = state->targets[t];
        const float resolved = Reduce(*state, t);
        const float pan2d = t == kTargetVoicePan ? Pan2dOf(*state) : 0.0f;
        if (!PublicationChanged(*state, t, resolved, pan2d)) {
            // Unchanged targets were never dirtied by the base write, so
            // there is nothing to settle and nothing to publish.
            continue;
        }
        if (written >= capacity) {
            // Defensive only (callers size to the requested list): an
            // unwritten value stays dirty for the ordinary flush rather
            // than being silently lost.
            SetTargetDirty(*state, target);
            continue;
        }
        out[written].targetId = t;
        out[written].resolved = resolved;
        out[written].pan2d = pan2d;
        ++written;
        RecordPublishedValue(*state, t, resolved, pan2d);
        // The primed target is settled: its demand marks retire and no
        // ramp is pending (initial instantiation snaps, section 9.2).
        ClearTargetPendingInput(target);
        SetTargetClean(target);
        target.pendingRampMs = 0;
    }
    return written;
}

void ParamSystem::AttachPublicationCapacity(PropertyPublisher* publisher,
                                            LogRing* log) {
    m_capacityPublisher = publisher;
    m_capacityLog = log;
}

bool ParamSystem::HasBinding(VnVoiceHandle handle,
                             std::uint32_t targetId) const noexcept {
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        if (m_bindings[i].handle == handle &&
            m_bindings[i].targetId == targetId) {
            return true;
        }
    }
    return false;
}

std::uint32_t ParamSystem::UniqueBoundTargets(VnVoiceHandle handle) const {
    std::uint32_t mask = 0;
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        if (m_bindings[i].handle == handle) {
            mask |= (1u << m_bindings[i].targetId);
        }
    }
    std::uint32_t count = 0;
    while (mask != 0) {
        mask &= mask - 1;
        ++count;
    }
    return count;
}

void ParamSystem::MarkCommitted(VnVoiceHandle handle, std::uint32_t targetId,
                                float resolved, std::uint32_t rampMs, float pan2d) {
    VoiceState* state = StateFor(handle);
    if (state == nullptr || targetId >= kBuiltinTargetCount) {
        return; // voice ended (or slot reused) after staging: nothing to clean
    }
    TargetState& target = state->targets[targetId];
    RecordPublishedValue(*state, targetId, resolved, pan2d);
    SetTargetClean(target);
    // The published command carried the aggregated ramp; the fold starts
    // fresh for the next change, and the carried ramp becomes the
    // attribution record.
    target.pendingRampMs = 0;
    target.lastPublishedRampMs = rampMs;
    ++m_commits;
}

ParamSystem::VoiceState*
ParamSystem::StateFor(VnVoiceHandle handle) noexcept {
    const std::uint32_t slot = SlotOf(handle);
    if (handle == 0 || slot >= m_voiceStates.size() ||
        m_voiceStates[slot].handle != handle || m_voiceStates[slot].retired) {
        return nullptr;
    }
    return &m_voiceStates[slot];
}

const ParamSystem::VoiceState*
ParamSystem::StateFor(VnVoiceHandle handle) const noexcept {
    const std::uint32_t slot = SlotOf(handle);
    if (handle == 0 || slot >= m_voiceStates.size() ||
        m_voiceStates[slot].handle != handle || m_voiceStates[slot].retired) {
        return nullptr;
    }
    return &m_voiceStates[slot];
}

ParamSystem::RetiringTarget* ParamSystem::FindRetiringTarget(
    VnVoiceHandle handle, std::uint32_t targetId,
    VariableSliceRef owner) noexcept {
    return const_cast<RetiringTarget*>(
        static_cast<const ParamSystem*>(this)->FindRetiringTarget(
            handle, targetId, owner));
}

const ParamSystem::RetiringTarget* ParamSystem::FindRetiringTarget(
    VnVoiceHandle handle, std::uint32_t targetId,
    VariableSliceRef owner) const noexcept {
    for (const RetiringTarget& retiring : m_retiringTargets) {
        if (retiring.used && retiring.handle == handle &&
            retiring.targetId == targetId &&
            retiring.owner.slot == owner.slot &&
            retiring.owner.generation == owner.generation) {
            return &retiring;
        }
    }
    return nullptr;
}

ParamSystem::Variable* ParamSystem::FindVariable(VnVariableId id) noexcept {
    for (std::uint32_t i = 0; i < m_variableCount; ++i) {
        if (m_variables[i].id == id) {
            return &m_variables[i];
        }
    }
    return nullptr;
}

const ParamSystem::Variable* ParamSystem::FindVariable(VnVariableId id) const noexcept {
    for (std::uint32_t i = 0; i < m_variableCount; ++i) {
        if (m_variables[i].id == id) {
            return &m_variables[i];
        }
    }
    return nullptr;
}

bool ParamSystem::ResolveScopedValue(const EmitterTable* emitters,
                                     VnEmitterHandle emitterHandle,
                                     VnVariableId variableId,
                                     float* out) const {
    // Emitter-local scope wins when the voice follows a live emitter that
    // holds an override for this Variable (LocalValue is false for a stale
    // emitter, so a destroyed emitter's voices fall back to global).
    if (emitters != nullptr && emitterHandle != 0 &&
        emitters->LocalValue(emitterHandle, variableId, out)) {
        return true;
    }
    const Variable* variable = FindVariable(variableId);
    if (variable == nullptr) {
        return false; // never set at either scope: no contribution
    }
    *out = variable->value;
    return true;
}

float ParamSystem::Reduce(const VoiceState& state,
                          std::uint32_t targetId) const {
    return Reduce(state, targetId, kCombineSlotCount, 0.0f);
}

float ParamSystem::Reduce(const VoiceState& state,
                          std::uint32_t targetId,
                          std::uint32_t overrideSlot,
                          float overrideValue) const {
    const TargetState& target = state.targets[targetId];
    const CombineDomain domain = DomainOfTarget(targetId);
    float value = state.base[targetId];
    float logContribution = 0.0f;
    // Fixed slot order (API direct, Variable bindings, attenuation, fades)
    // keeps the reduction deterministic; inactive slots contribute the
    // domain's neutral element by being skipped. An override substitutes
    // one slot without writing live state and is treated as active.
    for (std::uint32_t s = 0; s < kCombineSlotCount; ++s) {
        const bool useOverride = (s == overrideSlot);
        if (!useOverride && !target.active[s]) {
            continue;
        }
        const float contribution =
            useOverride ? overrideValue : target.slots[s];
        switch (domain) {
        case CombineDomain::AddDb:
        case CombineDomain::AddCents:
        case CombineDomain::Add:
            value += contribution;
            break;
        case CombineDomain::AddLogHz:
        case CombineDomain::AddLogQ:
            logContribution += contribution;
            break;
        case CombineDomain::MinHz:
            value = (contribution < value) ? contribution : value;
            break;
        case CombineDomain::MaxHz:
        case CombineDomain::Max:
            value = (contribution > value) ? contribution : value;
            break;
        }
    }
    if (domain == CombineDomain::AddLogHz ||
        domain == CombineDomain::AddLogQ) {
        value *= std::exp2(logContribution);
    }
    if (targetId == kTargetVoiceLowShelfFreqHz ||
        targetId == kTargetVoiceHighShelfFreqHz) {
        const float maximum = 0.499f * static_cast<float>(m_sampleRate);
        value = value > maximum ? maximum : value;
    }
    // Bounded targets (pan, width) clamp after the reduction: the clamp is
    // part of the declared model, so every consumer of a resolved value
    // sees the in-span number.
    return ClampResolvedTarget(targetId, value);
}

void ParamSystem::EvalDirtyBindings(const EmitterTable* emitters) {
    // Contributions accumulate INTO the Variable-bindings slot: several
    // bindings may feed one (voice, target), so when any of them changes the
    // whole slot rebuilds from every binding that feeds it, accumulating in
    // the target's domain. A binding "changes" when its resolved input — the
    // Variable value at the voice's emitter scope — moves since it last
    // evaluated, so global and emitter-local writes are both detected here
    // without per-Variable dirty flags, and an emitter-local write naturally
    // touches only that emitter's voices. Three passes over the (small, fixed)
    // binding table keep that correct without per-binding slot storage.

    // Pass 1: mark the (voice, target) slots whose feeding binding's resolved
    // input changed, and reset each marked slot to its domain-neutral empty
    // state. Over-marking is harmless — the flush's changed-only guard drops a
    // resolved value that did not actually move — but this marks precisely the
    // scopes that moved.
    bool anyRebuild = false;
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        Binding& binding = m_bindings[i];
        VoiceState* state = StateFor(binding.handle);
        if (state == nullptr) {
            continue; // the bound voice ended; the binding is inert
        }
        bool changed = false;
        if (binding.neutralized) {
            // A neutralized instance tracks nothing; its one exit
            // evaluation (contribution leaves the slot, ramp already
            // folded at neutralize time) is the only change it triggers.
            if (!binding.neutralizePending) {
                continue;
            }
            changed = true;
        } else {
            float input = 0.0f;
            const bool hasValue = ResolveScopedValue(
                emitters, state->emitterHandle, binding.variableId, &input);
            // An instance additionally re-evaluates when its definition's
            // revision moved: a live Replace changes the output at an
            // unchanged input (section 5.5 — every reader evaluates the
            // new revision from the install tick on).
            changed = !binding.everEvaluated || binding.definitionChanged ||
                      binding.lastHadValue != hasValue ||
                      (hasValue && binding.lastInput != input) ||
                      (binding.liveRevision != nullptr &&
                       binding.lastRevision != *binding.liveRevision);
            const std::uint32_t transition = BindingTransitionMs(binding);
            if (changed && transition != 0) {
                // Section 9.2 folds "every binding whose CONTRIBUTION
                // changed": an input move across a flat curve segment —
                // or a Replace to an equal-output curve — re-evaluates
                // but must not push its transition into the max, so the
                // fold compares the curve OUTPUT, not the input.
                const float contribution =
                    hasValue ? EvaluateBinding(binding, input) : 0.0f;
                const bool contributionChanged =
                    !binding.everEvaluated
                        ? hasValue
                        : (binding.lastHadValue != hasValue ||
                           (hasValue &&
                            contribution != binding.lastContribution));
                if (contributionChanged) {
                    TargetState& folded = state->targets[binding.targetId];
                    if (!folded.suppressClaimRamp &&
                        transition > folded.pendingRampMs) {
                        folded.pendingRampMs = transition;
                    }
                }
            }
        }
        if (!changed) {
            continue;
        }
        TargetState& target = state->targets[binding.targetId];
        if (!target.rebuild) {
            target.rebuild = true;
            target.active[kSlotVariableBindings] = false;
            target.slots[kSlotVariableBindings] = 0.0f;
            anyRebuild = true;
        }
    }
    if (!anyRebuild && m_forcedRebuilds == 0) {
        // No binding input moved and no purge left an exit behind: every
        // pending-input mark was a conservative over-mark; this
        // evaluation retires their future-demand claim.
        ConsumePendingInputMarks();
        return;
    }

    // Pass 2: accumulate every binding whose destination slot is being
    // rebuilt — including bindings whose own input did not change but which
    // still feed a rebuilt slot — evaluating each at its current scoped input
    // and recording it so the next tick detects the next change.
    for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
        Binding& binding = m_bindings[i];
        VoiceState* state = StateFor(binding.handle);
        if (state == nullptr) {
            continue;
        }
        TargetState& target = state->targets[binding.targetId];
        if (!target.rebuild) {
            continue;
        }
        if (binding.neutralized) {
            // The exit evaluation: the instance contributes nothing to
            // the rebuilt slot, its pending mark is consumed, and its
            // last realized contribution is now zero — the record the
            // attribution capture reports, which must keep summing to
            // the rebuilt slot (it held the pre-exit value only while
            // the slot still folded it).
            binding.neutralizePending = false;
            binding.lastContribution = 0.0f;
            continue;
        }
        float input = 0.0f;
        const bool hasValue = ResolveScopedValue(
            emitters, state->emitterHandle, binding.variableId, &input);
        binding.lastInput = input;
        binding.lastHadValue = hasValue;
        binding.everEvaluated = true;
        binding.definitionChanged = false;
        if (binding.liveRevision != nullptr) {
            binding.lastRevision = *binding.liveRevision;
        }
        if (!hasValue) {
            binding.lastContribution = 0.0f;
            continue; // no value at either scope: no contribution yet
        }
        const float contribution = EvaluateBinding(binding, input);
        binding.lastContribution = contribution;
        float& slot = target.slots[kSlotVariableBindings];
        if (!target.active[kSlotVariableBindings]) {
            slot = contribution;
            target.active[kSlotVariableBindings] = true;
            continue;
        }
        switch (DomainOfTarget(binding.targetId)) {
        case CombineDomain::AddDb:
        case CombineDomain::AddCents:
        case CombineDomain::Add:
        case CombineDomain::AddLogHz:
        case CombineDomain::AddLogQ:
            slot += contribution;
            break;
        case CombineDomain::MinHz:
            slot = (contribution < slot) ? contribution : slot;
            break;
        case CombineDomain::MaxHz:
        case CombineDomain::Max:
            slot = (contribution > slot) ? contribution : slot;
            break;
        }
    }

    // Pass 3: rebuilt slots make their targets commit candidates. The
    // purge-forced rebuilds were realized by the same passes, so their
    // gate resets with the flags.
    for (VoiceState& state : m_voiceStates) {
        for (std::uint32_t t = 0; t < kBuiltinTargetCount; ++t) {
            TargetState& target = state.targets[t];
            if (target.rebuild) {
                target.rebuild = false;
                SetTargetDirty(state, target);
            }
        }
    }
    m_forcedRebuilds = 0;
    // Every pending-input mark is consumed AFTER the rebuilds dirtied
    // their targets, so a realized change hands its census seat from
    // pending-input to dirty without a gap.
    ConsumePendingInputMarks();
}

void ParamSystem::ConsumePendingInputMarks() {
    if (m_pendingInputMarks == 0) {
        return;
    }
    for (VoiceState& state : m_voiceStates) {
        for (std::uint32_t t = 0; t < kBuiltinTargetCount; ++t) {
            ClearTargetPendingInput(state.targets[t]);
        }
        if (m_pendingInputMarks == 0) {
            return;
        }
    }
}

} // namespace harken
