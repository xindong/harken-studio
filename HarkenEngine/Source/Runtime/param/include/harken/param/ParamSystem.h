// Parameter system, logic-thread side: Variables (named game-facing
// inputs), Bindings (a curve mapping one Variable to one per-voice target),
// and the combine model that reduces layered contributions onto each
// target's authored base value.
//
// Everything here runs on the logic thread. Curve evaluation and the
// four-slot reduction happen at Vn_Update; only changed (target, resolved
// value) pairs cross to the audio thread as commands — the audio thread
// never evaluates curves.
//
// Combine model: each target owns four fixed contribution slots —
// API direct, Variable bindings, attenuation, fades — reduced in that
// order onto the base with the target's declared domain operation
// (add-in-dB, add-in-cents, min, max). Fixed slots, preallocated storage,
// deterministic order. The attenuation slot carries the engine's spatial
// contributions (distance-driven values and the engine-computed pan),
// written per tick by the spatializer before the flush; the fades slot
// exists but stays neutral while stop-fades remain a render-side
// mechanism.
//
// Binding assembly is an engine-internal C++ surface by design: the M1
// public API drives Variables only (Vn_SetValue); hosts and tests wire
// bindings directly, and a public registration channel arrives with the
// attenuation segment.
#ifndef HARKEN_PARAM_PARAM_SYSTEM_H
#define HARKEN_PARAM_PARAM_SYSTEM_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/core/EngineConfig.h"
#include "harken/core/LogRing.h"
#include "harken/core/VariableReadCut.h"
#include "harken/core/VariableSliceTable.h"
#include "harken/param/Curve.h"
#include "harken/param/ParamTargets.h"
#include "harken/param/PropertyPublisher.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace harken {

// Emitter-local Variable scope, consulted during binding evaluation. A voice
// attached to a live emitter resolves a Variable read to the emitter's local
// override when it has one, else to the global value. Forward-declared so the
// param system depends only on a pointer (isolated tests pass nullptr for
// global-only scope).
class EmitterTable;
class BindingTable;
class BindingDependencyIndex;
enum class BindingDependencyKind : std::uint32_t;
struct BindingDependencyIdentity;

// Fixed contribution slots per target, reduced in index order. The order
// is part of the model's determinism guarantee.
enum : std::uint32_t {
    kSlotApiDirect = 0,
    kSlotVariableBindings = 1,
    kSlotAttenuation = 2,
    kSlotFades = 3,
    kSlotMeterProbe = 4,
    kCombineSlotCount = 5
};

// Preallocated Variable table capacity. Variables are logic-side state,
// but their storage is still fixed at init so a running game never grows
// a container mid-session. The binding/instance table's capacity is
// config-driven (maxVoices x voiceBindingInstanceSlots): the historical
// fixed table is the instance layer's implementation detail, superseded
// by the config-driven binding pools.
inline constexpr std::uint32_t kMaxVariables = 256;

// Pitch crosses the combine model in cents and returns to the audio thread
// as a ratio: cents = 1200 * log2(ratio), ratio = 2^(cents / 1200).
inline double RatioToCents(double ratio) {
    return 1200.0 * std::log2(ratio);
}
inline double CentsToRatio(double cents) {
    return std::exp2(cents / 1200.0);
}

// One resolved value ready to travel to the audio thread. The value is in
// the target's combine domain (dB, cents, Hz); the API layer converts to
// the wire form (linear-domain command payloads) when it builds commands.
// rampMs is the target-level ramp aggregation: the max transitionMs over
// every binding whose contribution changed this tick, and the rampMs of
// an API-direct change if one
// occurred this tick. Only transition-capable targets ever carry a
// nonzero value (registration refuses transitions elsewhere), and the
// wire translation forwards it only where the command has a ramp field.
struct ParamCommit {
    VnVoiceHandle handle = 0;
    std::uint32_t targetId = 0;
    float resolved = 0.0f;
    std::uint32_t rampMs = 0;
    float pan2d = 0.0f;
};

// Live views into one binding definition's pool storage, handed to the
// instance layer when an instance is created. The pointers stay valid for
// the definition pool's lifetime (fixed-capacity storage, never
// reallocated); reading through them each evaluation is what makes a
// live Replace visible to every instance from its install tick on.
// variableId and targetId are the
// definition's FROZEN structural identity, copied by value.
struct BindingDefinitionView {
    VnObjectId definitionId{};
    VnVariableId variableId{};
    std::uint32_t targetId = 0;
    const Curve* curve = nullptr;
    const std::uint32_t* transitionMs = nullptr;
    const std::uint32_t* revision = nullptr;
    bool snapshotOwned = false;
};

// What the flush callback did with one handed-over commit. Retry means the
// commit was refused. Staged means the transactional publisher accepted it
// into the current batch: the target stays dirty until MarkCommitted, while
// the next gather advances beyond the accepted prefix. Committed means a
// command reached the queue. Dropped consumes the value without a command.
enum class FlushOutcome : std::uint8_t {
    Retry,
    Staged,
    Committed,
    Dropped
};

class ParamSystem {
    struct Binding;

public:
    explicit ParamSystem(const EngineConfig& config);
    ParamSystem(const ParamSystem& other);
    ParamSystem& operator=(const ParamSystem&) = delete;

    // Voice lifecycle, called by the API layer when a play succeeds. Resets
    // the reused slot's combine state and records the authored bases
    // (volume in dB, pitch as a ratio; the filters' bases are the fixed
    // neutral bypass values). emitterHandle (0 = none) is the emitter the
    // voice follows: it scopes this voice's Variable reads to that emitter's
    // local overrides during binding evaluation.
    void OnVoiceStarted(VnVoiceHandle handle, float baseVolumeDb,
                        double basePitchRatio, VnEmitterHandle emitterHandle = 0);
    void RetireVoiceTargets(VnVoiceHandle handle) noexcept;
    void DiscardPreparedVoice(VnVoiceHandle handle) noexcept;

    void ApplyTonePreset(VnVoiceHandle handle,
                         const VoiceTonePreset& preset);
    void ApplyProximityFrequency(VnVoiceHandle handle, float frequencyHz);

    // Writes the API-direct slot of one target and reduces immediately.
    // Returns the resolved domain value, which the caller commits as a
    // command in the same call (queue admission is pre-checked by the
    // caller, so the commit cannot fail after this writes). The target's
    // last-committed record updates here so the next flush does not
    // re-commit the same value. rampMs records the API-direct change for
    // this tick's target-level ramp aggregation: if binding
    // contributions on the same target also
    // change before the tick's flush, the flush's single command carries
    // max(changed bindings' transitionMs, this rampMs).
    float SetApiDirect(VnVoiceHandle handle, std::uint32_t targetId,
                       float domainValue, std::uint32_t rampMs = 0);

    // Writes or clears the attenuation slot of one target. The spatializer
    // calls this once per tick per spatialized voice; a write that changes
    // nothing (same value, slot already active) marks nothing dirty, so an
    // unmoved emitter commits nothing. Clearing an inactive slot is a
    // no-op. rampMs joins the target's existing max aggregation only when
    // the attenuation value changes. Unknown voices and targets are ignored.
    void SetAttenuationSlot(VnVoiceHandle handle, std::uint32_t targetId,
                            float domainValue, std::uint32_t rampMs = 0);
    void ClearAttenuationSlot(VnVoiceHandle handle, std::uint32_t targetId);
    void SetMeterProbeContribution(VnVoiceHandle handle,
                                   VnObjectId bindingId,
                                   std::uint32_t targetId, float value,
                                   std::uint32_t transitionMs);
    void ClearMeterProbeContribution(VnVoiceHandle handle,
                                     VnObjectId bindingId,
                                     std::uint32_t targetId,
                                     std::uint32_t transitionMs);

    class PreparedVariableToken {
    public:
        bool IsPrepared() const noexcept { return m_prepared; }
        std::uint32_t SlotIndex() const noexcept { return m_slotIndex; }
        bool AppendsSlot() const noexcept { return m_append; }
        bool ChangesValue() const noexcept { return m_changes; }
        VnVariableId VariableId() const noexcept { return m_id; }

    private:
        friend class ParamSystem;

        VnVariableId m_id{};
        float m_value = 0.0f;
        std::uint32_t m_slotIndex = 0;
        bool m_append = false;
        bool m_changes = false;
        bool m_prepared = false;
    };

    struct PreparedPurgeExit {
        VnVoiceHandle handle = 0;
        std::uint32_t targetId = 0;
        bool owedExit = false;
    };

    class PreparedPurgeToken {
    public:
        bool IsPrepared() const { return m_prepared; }
        std::uint32_t ExitCount() const { return m_exitCount; }

    private:
        friend class ParamSystem;
        const VnObjectId* m_definitionIds = nullptr;
        std::uint32_t m_definitionCount = 0;
        PreparedPurgeExit* m_exits = nullptr;
        std::uint32_t m_exitCount = 0;
        bool m_prepared = false;
    };

    // Vn_SetValue: refuses non-finite values; the first set of an unknown
    // id creates the Variable (VN_NO_VARIABLE when the table is full). A
    // value identical to the current one does not mark anything dirty. A
    // CHANGED value marks the targets of every binding on this Variable
    // as pending input immediately (NoteVariableWrite), so an active
    // deferral's admission reservation grows the moment the write lands;
    // evaluation remains the change-detection authority and is the only
    // consumer of the mark.
    VnResult SetValue(VnVariableId id, float value);
    VnResult SetInstalledValue(VnVariableId id, float value) noexcept;
    void ClearInstalledValues() noexcept;
    VnResult PrepareVariable(VnVariableId id, float value,
                             std::uint32_t slotIndex,
                             PreparedVariableToken* out) const;
    void CancelPreparedVariable(PreparedVariableToken* token) noexcept;
    void CommitPreparedVariable(PreparedVariableToken* token) noexcept;

    // Marks the bound targets of one Variable as pending-input NOW,
    // growing the live demand census the admission reservation reads.
    // SetValue calls it on every changed global write; the emitter-scope
    // write paths call it directly (scope precision is deliberately not
    // reproduced here — over-marking is harmless, under-reserving is
    // not). Bindings on dead voices are skipped. The pending-input mark
    // is consumed only by binding evaluation — a direct setter cannot
    // erase it, so demand known since a deferral survives until the
    // gather that realizes (or disproves) it.
    void NoteVariableWrite(VnVariableId id, VnEmitterHandle emitter = 0) noexcept;
    void NoteProbeChanged(VnObjectId stableId, std::uint32_t generation) noexcept;
    // Marks every bound target of every voice attached to `emitter` as
    // pending-input: destroying the emitter flips those voices' scoped
    // Variable resolution to the global fallback, a latent change the
    // next evaluation realizes. Called by the emitter-destroy path.
    void NoteEmitterDetached(VnEmitterHandle emitter);

    // Live demand census: unique (voice, target) marks that are dirty,
    // pending-input, or both — the size of the publication the next
    // gather stages if nothing else changes. During an active deferral
    // the admission gates reserve max(PendingReservation, this), so
    // growth known since the deferral extends the reservation before any
    // later single-command admission.
    std::uint32_t DirtyTargetCount() const { return m_dirtyTargetCount; }
    bool PendingPublication(VnVoiceHandle handle,
                            std::uint32_t targetId) const;
    float AuthoredBase(VnVoiceHandle handle, std::uint32_t targetId) const noexcept;
    float LastCommitted(VnVoiceHandle handle,
                        std::uint32_t targetId) const noexcept;

    // Engine-internal binding assembly: maps a Variable through a curve to
    // one built-in target of one voice. The curve is copied into
    // preallocated binding storage. Returns false on a full table, an
    // unknown target, an empty curve, or — with a publication capacity
    // attached — when adding a NEW unique (voice, target) pair would push
    // the worst-case one-tick publication past the command queue; the
    // capacity refusal logs a
    // record carrying both numbers and changes no state.
    class PreparedBindingToken {
    public:
        PreparedBindingToken() = default;
        ~PreparedBindingToken();
        PreparedBindingToken(const PreparedBindingToken&) = delete;
        PreparedBindingToken& operator=(const PreparedBindingToken&) = delete;
        PreparedBindingToken(PreparedBindingToken&& other) noexcept;
        PreparedBindingToken& operator=(PreparedBindingToken&& other) noexcept;
        bool IsPrepared() const noexcept { return m_owner != nullptr; }

    private:
        friend class ParamSystem;
        ParamSystem* m_owner = nullptr;
        std::uint32_t m_slot = UINT32_MAX;
        Binding* m_stagedBinding = nullptr;
    };

    // Attach before creating bindings. A preparation reserves backing only;
    // binding admission and every native identity remain with their original owner.
    bool AttachBindingDependencies(BindingDependencyIndex* dependencies) noexcept;
    bool PrepareBinding(PreparedBindingToken* out) noexcept;
    void CancelPreparedBinding(PreparedBindingToken* token) noexcept;
    // Staging borrows an already prepared, stable token until commit or discard.
    // The token must not move or be canceled while borrowed. Discard preserves it.
    bool BindVoiceTarget(VnVariableId variableId, VnVoiceHandle handle,
                         std::uint32_t targetId, const Curve& curve,
                         PreparedBindingToken* preparation = nullptr,
                         bool staged = false);
    void CommitStagedVoiceBindings(VnVoiceHandle handle) noexcept;

    // --- definition-backed binding instances: the M1 mechanism above
    // evolved to reference a
    // registered definition by id plus the attachment identity that
    // instantiated it. Instances hold NO curve copy: evaluation reads the
    // definition's current revision through the view's pointers, so a
    // live Replace is visible from its install tick on. ---

    enum class InstanceBindResult : std::uint8_t {
        Ok = 0,
        Duplicate,       // this definition already has an instance here
        SlotCeiling,     // the voice's instance slots are exhausted
        TableFull,       // the whole instance table is exhausted
        CapacityRefused, // section-10.4 worst-case publication bound
        InvalidCurve,    // snapshotOwned with no curve to copy
    };

    // Creates one instance of a definition on one voice.
    // attachmentObjectId is the identity of the voice-scope attachment
    // that instantiated it (the playable id), 0 with viaApply for the
    // per-voice apply verb. Instance identity is the FULL triple
    // (definitionId, attachmentObjectId, viaApply): one definition
    // attached to both a played container and its resolved leaf
    // instantiates twice and contributes twice — the literal meaning of
    // the attachment layer's fan-out (section 5.3) — and only an
    // exact-identity duplicate refuses. The per-voice ceiling and the
    // shared table bound both refuse before any state changes, as does
    // the section-10.4 capacity gate (same discipline as
    // BindVoiceTarget).
    InstanceBindResult BindVoiceInstance(const BindingDefinitionView& view,
                                         VnVoiceHandle handle,
                                         VnObjectId attachmentObjectId,
                                         bool viaApply,
                                         PreparedBindingToken* preparation = nullptr,
                                         bool staged = false);
    InstanceBindResult CanBindVoiceInstance(
        const BindingDefinitionView& view, VnVoiceHandle handle,
        VnObjectId attachmentObjectId, bool viaApply) const;

    // Neutralizes every instance born from one attachment identity
    // (definition id + object id, viaApply false): the contribution ramps
    // to neutral with the definition's transitionMs at the next
    // evaluation, and the instance stops tracking inputs. The slot stays
    // occupied until the voice ends (slot reuse) — the honest static
    // rule. Apply-born instances of the same definition are untouched
    // (different identity). Returns the number neutralized.
    std::uint32_t NeutralizeInstancesOf(VnObjectId definitionId,
                                        VnObjectId attachmentObjectId);

    // Neutralizes one voice's APPLY-born instance of one definition —
    // the exact inverse of the apply verb (identity: definitionId,
    // object 0, viaApply). Attachment-born instances of the same
    // definition are a different identity and stay live (detach owns
    // them). False when the voice holds no live apply instance of it.
    bool NeutralizeVoiceInstance(VnVoiceHandle handle,
                                 VnObjectId definitionId);

    // Removes every instance of a definition, live or inert (called after
    // a successful unregister, which the reference scan already proved
    // safe), so no instance can ever read a recycled definition slot.
    // A removed entry that still owed the slot a contribution exit (a
    // neutralize whose exit evaluation had not run yet, or a live
    // contribution) forces its target's slot to rebuild from the
    // surviving feeders at the next evaluation, so the departed
    // contribution publishes its exit instead of lingering forever
    // (section 5.3's detach-neutralizes rule survives a same-tick
    // unregister). Worst-case demand releases once per unique removed
    // (voice, target) pair whose last feeder left.
    void PurgeInstancesOf(VnObjectId definitionId);
    VnResult PreparePurgeInstancesOf(
        const VnObjectId* definitionIds,
        std::uint32_t definitionCount, PreparedPurgeExit* scratch,
        std::uint32_t scratchCapacity,
        PreparedPurgeToken* out) const noexcept;
    void CancelPreparedPurgeInstances(
        PreparedPurgeToken* token) noexcept;
    void CommitPreparedPurgeInstances(
        PreparedPurgeToken* token) noexcept;
    std::uint32_t BindingCapacity() const {
        return static_cast<std::uint32_t>(m_bindings.size());
    }
    std::uint32_t PreparationCapacity() const noexcept { return m_preparationCapacity; }

    // Removes ALL definition-backed instances of one voice WITHOUT
    // releasing their worst-case demand, returning the unique-target
    // count that demand represents (removed pairs no surviving entry of
    // the voice still feeds). The park path calls it after capturing the
    // instance identities: a parked logical voice still owns its
    // instances (section 5.6), so the demand transfers to the ParkRecord
    // as a held reservation — released at the parked voice's end, or
    // consumed by the return's re-binds (which grow back exactly this
    // amount, making the return's instance re-application infallible
    // against the section-10.4 bound).
    std::uint32_t PurgeVoiceInstances(VnVoiceHandle handle, bool snapshotOnly = false);

    // Marks every live instance of a definition as pending input: a
    // Replace installed this tick is known future demand exactly like a
    // Variable write, and the revision mismatch makes the next evaluation
    // re-evaluate the instance against the new curve.
    void NoteDefinitionReplaced(VnObjectId definitionId);
    bool RefreshPreviewBindings(VnVoiceHandle handle, const BindingTable& definitions,
                                bool allowPitch);

    // Live instances of one definition, for the unregister reference scan
    // (the caller filters engine-level voice liveness). Neutralized
    // instances are skipped — they no longer reference the curve.
    template <typename Fn>
    void ForEachInstanceOf(VnObjectId definitionId, Fn&& fn) const {
        for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
            const Binding& b = m_bindings[i];
            if (b.definitionId == definitionId && !b.neutralized) {
                fn(b.handle);
            }
        }
    }

    // Occupied instance slots of one voice (raw bindings and instances,
    // neutralized included — slots free only at voice end).
    std::uint32_t VoiceBindingCount(VnVoiceHandle handle) const;

    template <typename Fn>
    void ForEachVariableTarget(VnVariableId variableId, Fn&& fn) const {
        for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
            const Binding& binding = m_bindings[i];
            if (binding.variableId != variableId || binding.neutralized) {
                continue;
            }
            const VoiceState* state = StateFor(binding.handle);
            if (state == nullptr) {
                continue;
            }
            bool first = true;
            for (std::uint32_t prior = 0; prior < i; ++prior) {
                const Binding& earlier = m_bindings[prior];
                if (!earlier.neutralized &&
                    earlier.variableId == variableId &&
                    earlier.handle == binding.handle &&
                    earlier.targetId == binding.targetId) {
                    first = false;
                    break;
                }
            }
            if (first) {
                fn(binding.handle, state->emitterHandle, binding.targetId);
            }
        }
    }

    // Additive-domain sum of one voice's live instance contributions on
    // one target, evaluated at current definition revisions and current
    // scoped Variable values. The park path uses it to split the frozen
    // non-spatial term from the live instance term (volume) and the live
    // instance pitch cents; meaningful for add domains only.
    float InstanceContribution(VnVoiceHandle handle, std::uint32_t targetId,
                               const EmitterTable* emitters) const;

    // Binding+base volume a start would publish, evaluated now without
    // writing lastContribution or lastCommitted. False when the voice is
    // unknown; otherwise writes the reduced dB value even if it cannot
    // convert to a linear gain.
    bool TryPeekResolvedVolume(VnVoiceHandle handle,
                               const EmitterTable* emitters,
                               float* outDb,
                               const VariableReadCutReader* reader = nullptr) const;

    // One primed initial value bound for a voice's start batch.
    struct PrimedValue {
        std::uint32_t targetId = 0;
        float resolved = 0.0f;
        float pan2d = 0.0f;
    };

    // Evaluates every binding and instance of one voice NOW (first-block
    // correctness, sections 5.6/5.7): rebuilds the Variable-bindings
    // slots at current scoped values and current definition revisions,
    // records the change-detection state so the next flush sees no
    // spurious change, and writes into `out` every fed target whose
    // reduced value differs from its last-committed record — the
    // resolved initial targets the caller publishes in the SAME admitted
    // batch as the start command (rampMs 0: initial instantiation
    // snaps). The written targets' marks are cleaned here (lastCommitted
    // updated), so the tick's ordinary flush re-publishes nothing.
    // Returns the count written (bounded by kBuiltinTargetCount).
    std::uint32_t PrimeVoiceBindings(VnVoiceHandle handle,
                                     const EmitterTable* emitters,
                                     PrimedValue* out,
                                     std::uint32_t capacity);
    // Validates every captured input before changing any evaluator or publication state.
    bool TryPrimeVoiceBindings(VnVoiceHandle handle, const VariableReadCutReader& reader,
                               PrimedValue* out, std::uint32_t capacity,
                               std::uint32_t* written, bool includeUnchanged = false);

    // Settles an explicit list of targets for a voice's start batch, the
    // authored-base counterpart of PrimeVoiceBindings: each listed target
    // whose reduced value differs from its last-committed record is
    // written into `out` and marked committed and clean (rampMs 0 —
    // initial instantiation snaps), so the tick's ordinary flush
    // re-publishes nothing. Targets that resolve unchanged are skipped.
    // Returns the count written.
    std::uint32_t PrimeTargets(VnVoiceHandle handle,
                               const std::uint32_t* targetIds,
                               std::uint32_t count, PrimedValue* out,
                               std::uint32_t capacity);

    // Wires the section-10.4 capacity contract into the instance layer:
    // from here on, binding growth that adds a new unique (voice, target)
    // pair on a live voice consults the publisher's worst-case bound, and
    // slot reuse releases the dead occupant's pairs. One unique pair is
    // one worst-case commit — a target publishes at most one command per
    // tick regardless of how many bindings feed it. Null publisher (the
    // standalone-ParamSystem tests) leaves growth bounded only by the
    // table, as before.
    void AttachPublicationCapacity(PropertyPublisher* publisher,
                                   LogRing* log);

    // Evaluates bindings whose resolved input changed into the Variable-
    // bindings slots, reduces every dirty target, and hands changed resolved
    // values to `push` (signature FlushOutcome(const ParamCommit&)). Retry
    // and Staged keep the target dirty so the next update re-presents it;
    // Committed cleans the target and counts toward CommitCount; Dropped
    // cleans the target without counting — consumed is not committed.
    // Unchanged resolved values clear quietly.
    // Runs inside Vn_Update. `emitters` (nullptr = global-only scope) supplies
    // each attached voice's emitter-local Variable overrides.
    template <typename PushFn>
    void FlushCommits(PushFn&& push, const EmitterTable* emitters = nullptr) {
        EvalDirtyBindings(emitters);
        for (VoiceState& state : m_voiceStates) {
            state.anyDirty = false;
        }
        if (m_dirtyTargetCount == 0) {
            ++m_flushTick;
            return;
        }
        const std::size_t targetCount =
            m_voiceStates.size() * kBuiltinTargetCount;
        std::size_t nextCursor = m_flushCursor;
        bool stagedAny = false;
        for (std::size_t offset = 0; offset < targetCount; ++offset) {
            const std::size_t flat = (m_flushCursor + offset) % targetCount;
            VoiceState& state = m_voiceStates[flat / kBuiltinTargetCount];
            const std::uint32_t t =
                static_cast<std::uint32_t>(flat % kBuiltinTargetCount);
            TargetState& target = state.targets[t];
            if (state.handle == 0 || !target.dirty) {
                continue;
            }
            if (target.preparedOwner.Valid()) {
                state.anyDirty = true;
                continue;
            }
            const float resolved = Reduce(state, t);
            const float pan2d = t == kTargetVoicePan ? Pan2dOf(state) : 0.0f;
            if (!PublicationChanged(state, t, resolved, pan2d)) {
                SetTargetClean(target);
                target.pendingRampMs = 0;
                continue;
            }
            ParamCommit commit;
            commit.handle = state.handle;
            commit.targetId = t;
            commit.resolved = resolved;
            commit.pan2d = pan2d;
            commit.rampMs = target.pendingRampMs;
            if (target.apiRampTick == m_flushTick &&
                target.apiRampMs > commit.rampMs) {
                commit.rampMs = target.apiRampMs;
            }
            switch (push(commit)) {
            case FlushOutcome::Committed:
                RecordPublishedValue(state, t, resolved, pan2d);
                SetTargetClean(target);
                target.pendingRampMs = 0;
                target.lastPublishedRampMs = commit.rampMs;
                ++m_commits;
                break;
            case FlushOutcome::Dropped:
                RecordPublishedValue(state, t, resolved, pan2d);
                SetTargetClean(target);
                target.pendingRampMs = 0;
                break;
            case FlushOutcome::Staged:
                stagedAny = true;
                nextCursor = (flat + 1) % targetCount;
                [[fallthrough]];
            case FlushOutcome::Retry:
            default:
                target.pendingRampMs = commit.rampMs;
                state.anyDirty = true;
                break;
            }
        }
        if (stagedAny) {
            m_flushCursor = nextCursor;
        }
        // Advances the API-direct tick stamp window: setters that land
        // after this flush aggregate into the NEXT tick's publication.
        ++m_flushTick;
    }

    // Records the outcome of one published property commit: the resolved
    // value becomes the changed-only reference and the target is clean.
    // The transactional publication path calls this after whole-batch
    // admission — its gather stages commits under Staged so a deferred
    // batch keeps every target dirty; the marks clean only here, when a
    // command actually reached the queue. A handle whose voice ended
    // between deferral and retry is ignored (the pushed command is a
    // benign audio-side no-op and counts no commit). rampMs is the ramp
    // the committed command carried, recorded for attribution capture.
    void MarkCommitted(VnVoiceHandle handle, std::uint32_t targetId,
                       float resolved, std::uint32_t rampMs = 0,
                       float pan2d = 0.0f);

    // Reads a Variable at an emitter scope (emitter-local override when the
    // live emitter has one, else global), writing *out and returning true when
    // a value resolves. False when neither scope holds a value. The container
    // selection filter uses this so a match test reads the same scoped value a
    // binding would. `emitters` may be nullptr (global-only scope).
    bool ScopedVariableValue(const EmitterTable* emitters,
                             VnEmitterHandle emitterHandle,
                             VnVariableId variableId, float* out) const {
        return ResolveScopedValue(emitters, emitterHandle, variableId, out);
    }

    // The emitter a started voice follows (0 = none / unknown voice). A pure
    // read of the voice's recorded scope, so an effect-parameter binding can
    // resolve its Variable at the same emitter scope a built-in voice target
    // binding would.
    VnEmitterHandle EmitterHandleOf(VnVoiceHandle handle) const {
        const VoiceState* state = StateFor(handle);
        return state != nullptr ? state->emitterHandle : 0;
    }

    // Portable per-voice parameter state for park/return: the active API-direct
    // slot values and the RAW (definition-less) Variable-binding curves.
    // Bases are not captured (the caller restores them via OnVoiceStarted
    // from the original play). Definition-backed instances are captured
    // separately as IDENTITIES (CaptureVoiceInstances below), never as
    // curve copies — the section-5.6 ParkRecord amendment.
    struct ApiDirectState {
        std::uint32_t targetId = 0;
        float value = 0.0f;
    };
    struct BindingState {
        VnVariableId variableId{};
        std::uint32_t targetId = 0;
        Curve curve;
    };
    // One binding instance identity: the definition id plus the
    // attachment identity that instantiated it. What a ParkRecord stores
    // instead of curve copies, and what a return re-applies.
    struct InstanceState {
        VnObjectId definitionId{};
        VnObjectId attachmentObjectId{}; // 0 with viaApply set
        bool viaApply = false;
    };
    // Captures a voice's re-applyable parameter state (API-direct values
    // and raw curve-copy bindings only). On return the caller replays it
    // onto the new render slot: SetApiDirect for each ApiDirectState and
    // BindVoiceTarget for each BindingState, so the returned voice keeps
    // its setters and keeps tracking Variable changes. A stale handle
    // captures nothing (empty vectors).
    void CaptureVoiceState(VnVoiceHandle handle,
                           std::vector<ApiDirectState>* apiDirect,
                           std::vector<BindingState>* bindings) const;

    // Captures a voice's live (non-neutralized) definition-backed
    // instances as identities, for the ParkRecord (section 5.6: bindingId
    // + attachment identity, never curve copies).
    void CaptureVoiceInstances(VnVoiceHandle handle,
                               std::vector<InstanceState>* instances) const;

    // Current reduced value of a target in its combine domain. Test and
    // diagnostic surface; returns 0 for an unknown voice or target.
    float ResolvedValue(VnVoiceHandle handle, std::uint32_t targetId) const;
    // Same fold as ResolvedValue, but one combine slot is substituted
    // without writing live state. Inactive stored slots still contribute
    // nothing; the override is treated as active. Unknown voices, targets,
    // or slots reduce to 0.
    float ReduceWithSlot(VnVoiceHandle handle, std::uint32_t targetId,
                         std::uint32_t slot, float domainValue) const;
    std::uint32_t PendingRampMs(VnVoiceHandle handle,
                                std::uint32_t targetId) const;
    bool TryClaim(VnVoiceHandle handle, std::uint32_t targetId,
                  TargetClaimOwner owner, std::uint32_t expectedRevision) noexcept;
    bool CanClaim(VnVoiceHandle handle, std::uint32_t targetId,
                  TargetClaimOwner owner, std::uint32_t expectedRevision) const noexcept;
    bool BeginVariableProjection() noexcept;
    void RestoreVariableProjection() noexcept;
    std::uint64_t VariableProjectionStorageBytes() const noexcept;
    std::uint32_t TakePublicationRamp(VnVoiceHandle handle,
        std::uint32_t targetId, TargetClaimOwner owner) noexcept;
    void RestorePublicationRamp(VnVoiceHandle handle, std::uint32_t targetId,
        TargetClaimOwner owner, std::uint32_t rampMs) noexcept;
    bool CaptureCandidateBaseline(VnVoiceHandle handle,
                                  std::uint32_t targetId,
                                  TargetClaimOwner owner) noexcept;
    void ClearCandidateBaseline(VnVoiceHandle handle, std::uint32_t targetId,
                                TargetClaimOwner owner) noexcept;
    void DropClaimedPublication(VnVoiceHandle handle, std::uint32_t targetId,
                                TargetClaimOwner owner) noexcept;
    void DropClaimedBaseline(VnVoiceHandle handle, std::uint32_t targetId,
                             TargetClaimOwner owner) noexcept;
    void ReleaseClaim(VnVoiceHandle handle, std::uint32_t targetId,
                      TargetClaimOwner owner,
                      ClaimReleaseReason reason =
                          ClaimReleaseReason::RetryPreserve) noexcept;
    bool SnapshotForClaim(VnVoiceHandle handle, std::uint32_t targetId,
                          TargetClaimOwner owner, float* resolved,
                          std::uint32_t* rampMs,
                          std::uint32_t* revision, float* pan2d = nullptr) const noexcept;
    bool CaptureSnapshotForClaim(VnVoiceHandle handle,
                                 std::uint32_t targetId,
                                 TargetClaimOwner owner, float* resolved,
                                 std::uint32_t* rampMs,
                                 std::uint32_t* revision, float* pan2d = nullptr) noexcept;
    void SetClaimRampSuppressed(VnVoiceHandle handle,
                                std::uint32_t targetId,
                                TargetClaimOwner owner,
                                bool suppressed) noexcept;
    std::uint32_t TargetRevision(VnVoiceHandle handle,
                                 std::uint32_t targetId) const noexcept;
    void EvaluateDirtyBindings(const EmitterTable* emitters) {
        EvalDirtyBindings(emitters);
    }
    bool ForeignClaim(VnVoiceHandle handle,
                      std::uint32_t targetId) const noexcept;
    void MarkSliceCommitted(VnVoiceHandle handle, std::uint32_t targetId,
                            TargetClaimOwner owner, float resolved,
                            std::uint32_t rampMs, float pan2d = 0.0f) noexcept;
    std::uint32_t RetiringTargetCountForTests() const noexcept;

    std::uint64_t CommitCount() const { return m_commits; }
    std::uint32_t VariableCount() const { return m_variableCount; }

    // Fullest the shared binding/instance table has ever been (raw
    // bindings and definition-backed instances share the pool, so the
    // honest high water is the table census).
    std::uint64_t InstanceHighWater() const { return m_instanceHighWater; }

    // One voice target's capture facts (section 12), the attribution
    // row's voice-side source. handle is the RENDER handle — the caller
    // owns liveness filtering and public-handle translation. resolved is
    // the full combine reduction (attenuation and fades folded), the
    // value publication commits.
    struct TargetCaptureRow {
        VnVoiceHandle handle = 0;
        std::uint32_t targetId = 0;
        float base = 0.0f;
        bool apiActive = false;
        float apiValue = 0.0f;
        bool variableActive = false;
        float variableValue = 0.0f;
        float resolved = 0.0f;
        bool pendingPublication = false;
        std::uint32_t lastPublishedRampMs = 0;
        std::uint32_t feederCount = 0; // non-neutralized entries feeding it
    };

    // Walks every ACTIVE target of every started voice slot, slot order
    // then target-id order — a target is active when any combine slot is
    // active, a publication demand mark is set, or a binding feeds it.
    // fn(const TargetCaptureRow&).
    template <typename Fn>
    void ForEachActiveTarget(Fn&& fn) const {
        for (const VoiceState& state : m_voiceStates) {
            if (state.handle == 0) {
                continue;
            }
            for (std::uint32_t t = 0; t < kBuiltinTargetCount; ++t) {
                const TargetState& target = state.targets[t];
                std::uint32_t feeders = 0;
                for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
                    const Binding& b = m_bindings[i];
                    if (b.handle == state.handle && b.targetId == t &&
                        !b.neutralized) {
                        ++feeders;
                    }
                }
                bool anySlot = false;
                for (std::uint32_t s = 0; s < kCombineSlotCount; ++s) {
                    anySlot = anySlot || target.active[s];
                }
                if (!anySlot && !target.dirty && !target.pendingInput &&
                    feeders == 0) {
                    continue;
                }
                TargetCaptureRow row;
                row.handle = state.handle;
                row.targetId = t;
                row.base = state.base[t];
                row.apiActive = target.active[kSlotApiDirect];
                row.apiValue = target.slots[kSlotApiDirect];
                row.variableActive = target.active[kSlotVariableBindings];
                row.variableValue = target.slots[kSlotVariableBindings];
                row.resolved = Reduce(state, t);
                row.pendingPublication = target.dirty || target.pendingInput;
                row.lastPublishedRampMs = target.lastPublishedRampMs;
                row.feederCount = feeders;
                fn(row);
            }
        }
    }

    // One definition-backed instance's capture facts (section 12). Raw
    // (definition-less) bindings are engine-internal and carry no public
    // binding identity, so they are not enumerated here — their effect
    // still shows in the target rows' Variable slot. lastInput /
    // lastContribution are the values of the last realized evaluation,
    // consistent with the slot state the target rows report.
    struct InstanceCaptureRow {
        VnVoiceHandle handle = 0; // render handle, caller translates
        VnEmitterHandle emitterHandle = 0; // the voice's Variable scope
        VnObjectId definitionId{};
        VnObjectId attachmentObjectId{};
        bool viaApply = false;
        VnVariableId variableId{};
        std::uint32_t targetId = 0;
        std::uint32_t lastRevision = 0;
        std::uint32_t transitionMs = 0; // definition's current request
        bool neutralized = false;
        bool hadValue = false;
        float lastInput = 0.0f;
        float lastContribution = 0.0f;
    };

    // Walks every definition-backed instance entry in table order.
    // fn(const InstanceCaptureRow&).
    template <typename Fn>
    void ForEachInstance(Fn&& fn) const {
        for (std::uint32_t i = 0; i < m_bindingCount; ++i) {
            const Binding& b = m_bindings[i];
            if (b.definitionId == VnObjectId{}) {
                continue;
            }
            InstanceCaptureRow row;
            row.handle = b.handle;
            if (const VoiceState* state = StateFor(b.handle)) {
                row.emitterHandle = state->emitterHandle;
            }
            row.definitionId = b.definitionId;
            row.attachmentObjectId = b.attachmentObjectId;
            row.viaApply = b.viaApply;
            row.variableId = b.variableId;
            row.targetId = b.targetId;
            row.lastRevision = b.lastRevision;
            row.transitionMs = BindingTransitionMs(b);
            row.neutralized = b.neutralized;
            row.hadValue = b.lastHadValue;
            row.lastInput = b.lastInput;
            row.lastContribution = b.lastContribution;
            fn(row);
        }
    }

private:
    struct BindingEvaluation {
        float lastInput = 0.0f;
        float lastContribution = 0.0f;
        std::uint32_t lastRevision = 0;
        bool lastHadValue = false;
        bool everEvaluated = false;
        bool definitionChanged = false;
        bool neutralizePending = false;
    };
    struct TargetEvaluation {
        float variableSlot = 0.0f;
        std::uint32_t pendingRampMs = 0;
        bool variableActive = false;
        bool suppressClaimRamp = false;
        bool dirty = false;
        bool pendingInput = false;
        bool rebuild = false;
    };
    struct VoiceEvaluation {
        VnVoiceHandle handle = 0;
        bool anyDirty = false;
        TargetEvaluation targets[kBuiltinTargetCount]{};
    };
    struct Variable {
        VnVariableId id{};
        float value = 0.0f;
    };

    struct Binding {
        VnVariableId variableId{};
        VnVoiceHandle handle = 0;
        std::uint32_t targetId = 0;
        VnEmitterHandle dependencyEmitter = 0;
        std::uint32_t dependencySlot = UINT32_MAX;
        std::uint32_t dependencyGeneration = 0;
        PreparedBindingToken* preparation = nullptr;
        // Raw (definition-less) and snapshotOwned instances copy their
        // curve here. Live definition-backed instances leave it empty
        // and evaluate through liveCurve instead. snapshotOwned must
        // not point liveCurve at this copy: compaction moves Binding
        // entries by assignment and would dangle the old address.
        Curve curve;
        // Definition-backed instance identity and live views (null for a
        // raw or snapshotOwned binding). Live pointers reference the
        // definition pool's fixed storage; a Replace installs into that
        // storage, so every evaluation reads the current revision.
        VnObjectId definitionId{};
        VnObjectId attachmentObjectId{};
        bool viaApply = false;
        const Curve* liveCurve = nullptr;
        const std::uint32_t* liveTransitionMs = nullptr;
        const std::uint32_t* liveRevision = nullptr;
        bool snapshotOwned = false;
        std::uint32_t ownedTransitionMs = 0;
        std::uint32_t lastRevision = 0;
        std::uint32_t previewRevision = 0;
        bool definitionChanged = false;
        // A neutralized instance contributes nothing and stops tracking
        // inputs; its slot frees when the voice ends. neutralizePending
        // marks the one evaluation that realizes the contribution's exit
        // (ramping to neutral with the definition's transitionMs).
        bool neutralized = false;
        bool neutralizePending = false;
        Binding* nextNeutralization = nullptr;
        // Change detection is per binding, not per Variable, because a
        // binding's contribution depends on the Variable value resolved at its
        // voice's emitter scope (emitter-local or global): the same global
        // change moves some voices' bindings and not others. A binding is
        // re-evaluated when its resolved input, or whether it has a value at
        // all, differs from what it last evaluated with — or, for an
        // instance, when the definition's revision moved (live Replace).
        // lastContribution is the curve OUTPUT it last evaluated to (0
        // while lastHadValue is false): section 9.2 folds a binding's
        // transitionMs only when its CONTRIBUTION changed, and a flat
        // curve segment (or an equal-output Replace) moves the input
        // without moving the output — detection stays conservative,
        // the ramp fold stays exact.
        float lastInput = 0.0f;
        float lastContribution = 0.0f;
        bool lastHadValue = false;
        bool everEvaluated = false;
    };

    struct TargetState {
        float slots[kCombineSlotCount] = {};
        bool active[kCombineSlotCount] = {};
        bool suppressClaimRamp = false;
        bool claimSnapshotCaptured = false;
        float lastCommitted = 0.0f;
        bool dirty = false;
        // A latent input change (Variable write, new binding, emitter
        // detach) whose effect the next binding evaluation will realize.
        // Counts toward the demand census like dirty, but only evaluation
        // consumes it — a direct setter cleaning its own dirty state
        // cannot erase demand the next gather is already known to carry.
        bool pendingInput = false;
        // Transient mark used while rebuilding the Variable-bindings slot:
        // set when any contributing Variable changed, cleared by the same
        // flush.
        bool rebuild = false;
        // Section-9.2 ramp aggregation state. pendingRampMs folds the max
        // transitionMs of every changed contributor since the target last
        // published (it survives a deferred publication with the dirty
        // mark). apiRampMs/apiRampTick record an API-direct change's
        // rampMs, valid only for the flush whose tick stamp matches.
        std::uint32_t pendingRampMs = 0;
        std::uint32_t apiRampMs = 0;
        std::uint32_t preparedRampMs = 0;
        std::uint64_t apiRampTick = 0;
        std::uint32_t preCandidatePendingRampMs = 0;
        std::uint32_t preCandidateApiRampMs = 0;
        std::uint64_t preCandidateApiRampTick = 0;
        bool candidateBaselineValid = false;
        // The rampMs the most recent committed command carried (direct
        // setters included; 0 before any). Attribution capture reads it;
        // nothing else does.
        std::uint32_t lastPublishedRampMs = 0;
        std::uint32_t revision = 1;
        TargetClaimOwner preparedOwner{};
        std::uint32_t publicationRefCount = 0;
    };

    struct MeterProbeContribution {
        bool used = false;
        VnVoiceHandle handle = 0;
        VnObjectId bindingId{};
        std::uint32_t targetId = 0;
        float value = 0.0f;
    };

    struct VoiceState {
        VnVoiceHandle handle = 0; // 0 = slot never started a voice
        VnEmitterHandle emitterHandle = 0; // 0 = none; scopes Variable reads
        float base[kBuiltinTargetCount] = {};
        bool lowShelfFrequencyAuthoredByTone = false;
        TargetState targets[kBuiltinTargetCount];
        bool anyDirty = false;
        bool retired = false;
        float lastCommittedPan2d = 0.0f;
    };

    struct RetiringTarget {
        bool used = false;
        bool claimSnapshotCaptured = false;
        VnVoiceHandle handle = 0;
        std::uint32_t targetId = 0;
        TargetClaimOwner owner{};
        std::uint32_t publicationRefCount = 0;
        std::uint32_t revision = 0;
        float frozenResolved = 0.0f;
        float frozenPan2d = 0.0f;
        std::uint32_t frozenRampMs = 0;
    };

    VoiceState* StateFor(VnVoiceHandle handle) noexcept;
    const VoiceState* StateFor(VnVoiceHandle handle) const noexcept;
    RetiringTarget* FindRetiringTarget(VnVoiceHandle handle,
                                       std::uint32_t targetId,
                                       TargetClaimOwner owner) noexcept;
    const RetiringTarget* FindRetiringTarget(
        VnVoiceHandle handle, std::uint32_t targetId,
        TargetClaimOwner owner) const noexcept;
    Variable* FindVariable(VnVariableId id) noexcept;
    const Variable* FindVariable(VnVariableId id) const noexcept;
    struct PrimingInput {
        float value = 0.0f;
        bool hasValue = false;
    };
    std::uint32_t PrimeVoiceBindingsImpl(VnVoiceHandle handle,
                                         const EmitterTable* emitters,
                                         PrimedValue* out, std::uint32_t capacity,
                                         const PrimingInput* capturedInputs, bool includeUnchanged = false);
    // Resolves a Variable at a voice's emitter scope: the emitter-local
    // override when the (live) emitter has one, else the global value. Returns
    // false when neither scope holds a value (the binding contributes nothing).
    bool ResolveScopedValue(const EmitterTable* emitters,
                            VnEmitterHandle emitterHandle,
                            VnVariableId variableId, float* out) const;
    float Reduce(const VoiceState& state, std::uint32_t targetId) const;
    float Reduce(const VoiceState& state, std::uint32_t targetId,
                 std::uint32_t overrideSlot, float overrideValue) const;
    float Pan2dOf(const VoiceState& state) const {
        return Reduce(state, kTargetVoicePan, kSlotAttenuation, 0.0f);
    }
    bool PublicationChanged(const VoiceState& state, std::uint32_t targetId,
                            float resolved, float pan2d) const {
        return state.targets[targetId].lastCommitted != resolved ||
               (targetId == kTargetVoicePan && state.lastCommittedPan2d != pan2d);
    }
    void RecordPublishedValue(VoiceState& state, std::uint32_t targetId,
                              float resolved, float pan2d) {
        state.targets[targetId].lastCommitted = resolved;
        if (targetId == kTargetVoicePan) state.lastCommittedPan2d = pan2d;
    }
    void EvalDirtyBindings(const EmitterTable* emitters);
    void EvalIndexedBindings(const EmitterTable* emitters);
    static BindingDependencyIdentity ReadBindingDependency(
        const void* fact, BindingDependencyKind kind) noexcept;
    void RegisterBindingDependency(Binding& binding, PreparedBindingToken* preparation) noexcept;
    void RemoveBindingDependency(Binding& binding) noexcept;
    void RemovePendingNeutralization(Binding& binding) noexcept;
    void MoveBinding(std::uint32_t from, std::uint32_t to) noexcept;
    // Whether any binding already feeds (handle, targetId) — a duplicate
    // pair adds no worst-case demand.
    bool HasBinding(VnVoiceHandle handle,
                    std::uint32_t targetId) const noexcept;
    float EvaluateBinding(const Binding& binding, float input) const;
    std::uint32_t BindingTransitionMs(const Binding& binding) const {
        if (binding.snapshotOwned) {
            return binding.ownedTransitionMs;
        }
        return binding.liveTransitionMs != nullptr ? *binding.liveTransitionMs
                                                   : 0u;
    }
    // Shared admission for both bind paths: the per-voice slot ceiling,
    // the table bound, and the section-10.4 worst-case gate, checked in
    // that order with zero side effects on refusal.
    InstanceBindResult AdmitBindingSlot(VnVoiceHandle handle,
                                        std::uint32_t targetId);
    // Marks one instance neutralized: contribution exits at the next
    // evaluation, ramped with the definition's transitionMs.
    void NeutralizeEntry(Binding& binding);
    // The single transition points for a target's demand marks: every
    // dirty/pending-input flip goes through these four, so
    // m_dirtyTargetCount stays an exact census of the UNION (a target
    // counts once whether dirty, pending-input, or both — the admission
    // reservation reads it live). OnVoiceStarted routes a reset slot's
    // marks through these too — no path writes the flags directly.
    void SetTargetDirty(VoiceState& state,
                        TargetState& target) noexcept {
        if (!target.dirty && !target.pendingInput) {
            ++m_dirtyTargetCount;
        }
        target.dirty = true;
        state.anyDirty = true;
    }
    // Cleans the plain dirty state a setter or a published commit owns.
    // A pending-input target stays counted: its demand belongs to the
    // next evaluation, not to the cleaner.
    void SetTargetClean(TargetState& target) {
        if (target.dirty && !target.pendingInput) {
            --m_dirtyTargetCount;
        }
        target.dirty = false;
    }
    void SetTargetPendingInput(TargetState& target) noexcept {
        if (target.pendingInput) {
            return;
        }
        if (!target.dirty) {
            ++m_dirtyTargetCount;
        }
        target.pendingInput = true;
        ++m_pendingInputMarks;
    }
    void ClearTargetPendingInput(TargetState& target) {
        if (!target.pendingInput) {
            return;
        }
        if (!target.dirty) {
            --m_dirtyTargetCount;
        }
        target.pendingInput = false;
        --m_pendingInputMarks;
    }
    // Consumes every pending-input mark after an evaluation pass has
    // observed current inputs: real changes were realized as dirty
    // rebuilt targets; the rest were conservative over-marks whose
    // future-demand claim ends here. Guarded sweep — zero marks, zero
    // work.
    void ConsumePendingInputMarks();
    // Unique targets bound on one voice handle, for the symmetric
    // worst-case release when its slot is reused.
    std::uint32_t UniqueBoundTargets(VnVoiceHandle handle) const;

    std::vector<VoiceState> m_voiceStates; // indexed by voice slot
    std::vector<RetiringTarget> m_retiringTargets;
    std::uint32_t m_sampleRate = 0;
    std::vector<Variable> m_variables;     // fixed capacity, m_variableCount live
    // Fixed capacity (maxVoices x voiceBindingInstanceSlots — the
    // section-16 instance pool), m_bindingCount live entries in stable
    // order. Raw bindings and definition-backed instances share it; a
    // slot-reuse purge reclaims the dead occupant's entries.
    std::vector<Binding> m_bindings;
    BindingDependencyIndex* m_dependencies = nullptr;
    Binding* m_pendingNeutralizations = nullptr;
    std::vector<std::uint32_t> m_freeDependencySlots;
    std::uint32_t m_dependencySlotCount = 0;
    std::uint32_t m_preparedBindingCount = 0;
    std::uint32_t m_preparationCapacity = 0;
    std::vector<PrimingInput> m_primingInputs;
    std::vector<Variable> m_projectionVariables;
    std::vector<BindingEvaluation> m_projectionBindings;
    std::vector<VoiceEvaluation> m_projectionVoices;
    bool m_projectionActive = false;
    std::uint32_t m_projectionVariableCount = 0;
    std::uint32_t m_projectionBindingCount = 0;
    std::uint32_t m_projectionForcedRebuilds = 0;
    std::uint32_t m_projectionDirtyTargetCount = 0;
    std::uint32_t m_projectionPendingInputMarks = 0;
    std::vector<MeterProbeContribution> m_meterProbeContributions;
    std::uint32_t m_instanceSlotsPerVoice = 0; // per-voice ceiling
    std::uint32_t m_variableCount = 0;
    std::uint32_t m_bindingCount = 0;
    std::uint64_t m_instanceHighWater = 0;
    std::uint64_t m_commits = 0;
    // Flush tick stamp scoping API-direct ramp records to their tick;
    // starts above the TargetState default so a never-stamped target can
    // never alias the first tick.
    std::uint64_t m_flushTick = 1;
    // Fixed-capacity round-robin start for over-capacity dirty sets.
    std::size_t m_flushCursor = 0;
    // Targets whose Variable-bindings slot a purge marked for rebuild
    // (their departed feeder's exit must publish even though no surviving
    // binding reports a change). Gates the evaluation's early-out so a
    // purge-only tick still runs the rebuild passes; reset there.
    std::uint32_t m_forcedRebuilds = 0;
    // Live demand census — union of dirty and pending-input marks; see
    // DirtyTargetCount(). m_pendingInputMarks counts pending-input flags
    // alone, guarding the consume sweep.
    std::uint32_t m_dirtyTargetCount = 0;
    std::uint32_t m_pendingInputMarks = 0;
    // Section-10.4 wiring (AttachPublicationCapacity): the worst-case
    // bound consulted at binding growth, and the ring capacity refusals
    // log through. Both null when no capacity is attached.
    PropertyPublisher* m_capacityPublisher = nullptr;
    LogRing* m_capacityLog = nullptr;
};

} // namespace harken

#endif // HARKEN_PARAM_PARAM_SYSTEM_H
