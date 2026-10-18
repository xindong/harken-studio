// Effect-parameter binding resolution store, logic-thread side. Realizes the
// VN_TARGET_SCOPE_VOICE_EFFECT_PARAM / VN_TARGET_SCOPE_BUS_EFFECT_PARAM combine
// model without a parallel binding API: it reuses the M6 direct-value-as-base
// composition (BusTargetStore precedent) but keys records by the packed effect-
// parameter locator (effect type, fx slot, parameter index) instead of a fixed
// bus x target grid, and takes each parameter's combination domain / neutral /
// clamp from the generated effect-parameter metadata authority rather than the
// voice/bus target table.
//
// One record per installed (object, fx slot, parameter index). A record is
// created when an effect installs (base seeded from the values the install
// applied, so a binding attached to a never-directly-set parameter composes on
// the authored descriptor value), updated when a direct Vn_Set*EffectParam
// retargets its base, folded from binding contributions each evaluation, and
// removed on clear / in-place replace / voice-end / recycle so a stale base can
// never leak to the slot's next occupant. Reduction composes the base with the
// folded contribution per the declared domain and clamps to the parameter's
// validated range (Nyquist bound resolved from the sample rate); the resolved
// value travels to the audio thread on the existing opcode 14/22, so a
// parameter with no binding keeps the direct path's single-command behavior.
//
// Effect parameters declare no transition support (the reused opcode is a
// block-boundary step), so this store carries no section-9.2 ramp aggregation —
// only the dirty / last-committed publication bookkeeping the transactional
// publisher needs.
#ifndef HARKEN_PARAM_EFFECT_PARAM_TARGET_STORE_H
#define HARKEN_PARAM_EFFECT_PARAM_TARGET_STORE_H

#include <harken/ObjectId.h>
#include "harken/core/VariableSliceTable.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/render/generated/harken_effect_registry.g.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace harken {

class EffectParamTargetStore {
public:
    // Object scope of a record: which host namespace objectId lives in. Kept
    // distinct so a voice render handle and a bus index never collide.
    enum class Scope : std::uint8_t { Voice, Bus };
    enum class Lifecycle : std::uint8_t { Free, Live, Retiring };

    static constexpr std::uint32_t kPreparedSeedMaxParams = 64;

    class PreparedSeedToken {
    public:
        bool IsPrepared() const noexcept { return m_prepared; }

    private:
        friend class EffectParamTargetStore;

        bool m_prepared = false;
        Scope m_scope = Scope::Voice;
        std::uint64_t m_objectId = 0;
        std::uint32_t m_objectGeneration = 0;
        std::uint32_t m_slot = 0;
        std::uint32_t m_effectType = 0;
        std::uint32_t m_paramCount = 0;
        std::uint32_t m_revision = 0;
        std::array<std::uint32_t, kPreparedSeedMaxParams> m_recordIndices{};
        std::uint32_t m_slotRecordCount = 0;
        std::array<std::uint32_t, kPreparedSeedMaxParams> m_slotRecordIndices{};
        std::array<float, kPreparedSeedMaxParams> m_paramValues{};
    };

    // sampleRate resolves the Nyquist clamp for frequency parameters; capacity
    // is the maximum number of concurrently-installed (object, slot, parameter)
    // records (preallocated, never grown after construction).
    EffectParamTargetStore(std::uint32_t sampleRate, std::uint32_t capacity,
                           std::uint32_t retiringCapacity = 0);

    // --- effect lifecycle (install / clear) ---

    // Seeds one installed effect's parameter records. paramValues[0, paramCount)
    // are the values the install applied (EffectParamsFromDesc order == index
    // order). Existing records for the same (scope, objectId, slot) are replaced
    // in place (in-place effect replace re-seeds). Records for parameters whose
    // metadata domain is None are still created (they hold the base) but never
    // accept a binding. Returns false if capacity is exhausted (no partial
    // seeding — all or nothing).
    bool SeedInstalledEffect(Scope scope, std::uint64_t objectId,
                             std::uint32_t slot, std::uint32_t effectType,
                             const float* paramValues,
                             std::uint32_t paramCount);
    bool SeedInstalledEffect(Scope scope, std::uint64_t objectId,
                             std::uint32_t objectGeneration,
                             std::uint32_t slot, std::uint32_t effectType,
                             const float* paramValues,
                             std::uint32_t paramCount);
    bool CanSeedFreshEffects(std::uint32_t paramCount) const;
    bool CanSeedInstalledEffect(Scope scope, std::uint64_t objectId,
                                std::uint32_t slot,
                                std::uint32_t effectType,
                                std::uint32_t paramCount) const;
    bool CanSeedInstalledEffect(Scope scope, std::uint64_t objectId,
                                std::uint32_t objectGeneration,
                                std::uint32_t slot,
                                std::uint32_t effectType,
                                std::uint32_t paramCount) const;
    bool PrepareInstalledEffectSeed(Scope scope, std::uint64_t objectId,
                                    std::uint32_t slot,
                                    std::uint32_t effectType,
                                    const float* paramValues,
                                    std::uint32_t paramCount,
                                    PreparedSeedToken* outToken);
    bool PrepareInstalledEffectSeed(Scope scope, std::uint64_t objectId,
                                    std::uint32_t objectGeneration,
                                    std::uint32_t slot,
                                    std::uint32_t effectType,
                                    const float* paramValues,
                                    std::uint32_t paramCount,
                                    PreparedSeedToken* outToken);
    void CancelPreparedInstalledEffectSeed(
        PreparedSeedToken* token) noexcept;
    void CommitPreparedInstalledEffectSeed(
        PreparedSeedToken* token) noexcept;
    bool ReplacePreparedInstalledEffectSeed(
        PreparedSeedToken* token, std::uint32_t effectType,
        const float* paramValues, std::uint32_t paramCount);
    bool PrimeContribution(Scope scope, std::uint64_t objectId,
                           std::uint32_t slot, std::uint32_t paramIndex,
                           std::uint32_t effectType, float contribution,
                           float* outResolved);
    void SetSeedFailureForTests(bool enabled) {
        m_failSeedForTests = enabled;
    }
    void SetPrepareSeedFailureAfterForTests(
        std::int32_t successfulPreparations) {
        m_prepareSeedFailureAfterForTests = successfulPreparations;
    }
    std::uint32_t ReservedRecordCountForTests() const {
        std::uint32_t count = 0;
        for (const Record& record : m_records) {
            if (record.reserved) {
                ++count;
            }
        }
        return count;
    }

    // Removes every record of one (scope, objectId, slot): effect clear or
    // in-place replace's old instance. Any binding contribution the records held
    // is dropped with them (the binding is expected to have been detached/neut-
    // ralized by the same path; a lingering record must never outlive its slot).
    void ClearSlot(Scope scope, std::uint64_t objectId, std::uint32_t slot);
    void ClearSlot(Scope scope, std::uint64_t objectId,
                   std::uint32_t objectGeneration, std::uint32_t slot);

    // Removes every record of one object (voice-end / recycle): all fx slots.
    void ClearObject(Scope scope, std::uint64_t objectId);
    void ClearObject(Scope scope, std::uint64_t objectId,
                     std::uint32_t objectGeneration);

    // --- direct value (Vn_Set*EffectParam) ---

    // Unbound direct set: retargets the base and adopts the value the direct
    // call published as the changed-only reference (that call emits the command
    // itself), leaving the record clean. No-op for a parameter with no record.
    void NoteDirect(Scope scope, std::uint64_t objectId, std::uint32_t slot,
                    std::uint32_t paramIndex, float baseValue,
                    float committedResolved);

    // Bound direct set: retargets the base and marks the record dirty WITHOUT
    // publishing — the composed value flows out through the next fold + FlushDirty
    // so a bound parameter never double-emits (the direct call must not also emit
    // 14/22). No-op for a parameter with no record.
    void NoteDirectBound(Scope scope, std::uint64_t objectId, std::uint32_t slot,
                         std::uint32_t paramIndex, float baseValue);

    // --- attach eligibility ---

    // Whether an installed record exists for a locator and its parameter accepts
    // a binding (installed effect type matches the locator's expected type, the
    // parameter index is in range, and its domain is not None). The attach path
    // consults this to refuse an empty slot, a type mismatch, an out-of-range
    // index, or a None-domain parameter with zero side effects.
    bool CanBind(Scope scope, std::uint64_t objectId,
                 const EffectParamLocator& loc) const;

    // --- binding contribution sweep (per Vn_Update evaluation) ---
    //
    // One sweep per update rebuilds every bound parameter's folded contribution:
    // BeginContributionSweep marks all records un-folded; AccumulateBinding folds
    // each live binding's curve output into its record (lazily resetting the
    // record to its domain neutral on the first fold of the sweep, so multiple
    // bindings on one parameter accumulate and grouping is automatic);
    // EndContributionSweep commits the folded value to each record (marking it
    // dirty if it changed) and returns any record whose bindings all detached to
    // the neutral contribution (the detach edge -> resolved returns to base).
    void BeginContributionSweep();
    // Folds one binding's curve output into the located record. effectType is
    // the locator's EXPECTED type: if the installed effect at the slot is a
    // different type (a stale/replaced slot), the contribution is skipped, so a
    // mismatched binding folds nothing and the parameter stays at its base.
    void AccumulateBinding(Scope scope, std::uint64_t objectId,
                           std::uint32_t slot, std::uint32_t paramIndex,
                           std::uint32_t effectType, float curveOutput);
    void EndContributionSweep();
    void BeginMeterProbeSweep();
    void AccumulateMeterProbe(Scope scope, std::uint64_t objectId,
                              std::uint32_t slot, std::uint32_t paramIndex,
                              std::uint32_t effectType,
                              VnObjectId bindingId, float value);
    void EndMeterProbeSweep();
    void ClearMeterProbeContribution(Scope scope, std::uint64_t objectId,
                                     std::uint32_t slot,
                                     std::uint32_t paramIndex,
                                     std::uint32_t effectType,
                                     VnObjectId bindingId);

    // --- reduction ---

    // Composes base with the folded contribution per the record's domain and
    // clamps to its validated range. Unknown records reduce to 0.
    float Reduce(Scope scope, std::uint64_t objectId, std::uint32_t slot,
                 std::uint32_t paramIndex) const;

    // --- publication ---

    // Flush walk over dirty records. For each dirty record whose reduction
    // differs from its last committed value, calls fn(scope, objectId, slot,
    // paramIndex, resolved); fn returns true when the command reached the queue,
    // in which case the record adopts the value and cleans. fn returning false
    // (queue full) leaves the record dirty so it retries next update — no silent
    // loss. A record whose reduction equals its last committed value cleans
    // quietly and calls nothing.
    template <typename Fn>
    void FlushDirty(Fn&& fn) {
        for (Record& r : m_records) {
            if (r.lifecycle != Lifecycle::Live || !r.dirty) {
                continue;
            }
            if (r.preparedOwner.Valid()) {
                continue;
            }
            const float resolved = ReduceRecord(r);
            if (resolved == r.lastCommitted) {
                CleanDirty(r);
                continue;
            }
            if (fn(r.scope, r.objectId, r.slot, r.paramIndex, resolved)) {
                r.lastCommitted = resolved;
                CleanDirty(r);
            }
            // else: push refused this update; stays dirty and retries next.
        }
    }

    std::uint32_t DemandCount() const { return m_dirtyCount; }
    bool PendingPublication(Scope scope, std::uint64_t objectId,
                            std::uint32_t slot,
                            std::uint32_t paramIndex) const;
    // Logic-side capacity accounting for lifecycle verification and internal
    // observability. Counts only live parameter records; cleared slots remain
    // reusable and do not contribute.
    std::uint32_t LiveRecordCount() const { return CountLive(); }
    float LastCommitted(Scope scope, std::uint64_t objectId, std::uint32_t slot,
                        std::uint32_t paramIndex) const;
    bool TryClaim(Scope scope, std::uint64_t objectId, std::uint32_t slot,
                  std::uint32_t paramIndex, TargetClaimOwner owner,
                  std::uint32_t expectedRevision) noexcept;
    bool TryClaim(Scope scope, std::uint64_t objectId,
                  std::uint32_t objectGeneration, std::uint32_t slot,
                  std::uint32_t paramIndex, TargetClaimOwner owner,
                  std::uint32_t expectedRevision) noexcept;
    void ReleaseClaim(Scope scope, std::uint64_t objectId, std::uint32_t slot,
                      std::uint32_t paramIndex,
                      TargetClaimOwner owner) noexcept;
    bool CanClaim(Scope scope, std::uint64_t objectId,
                  std::uint32_t objectGeneration, std::uint32_t slot,
                  std::uint32_t paramIndex, TargetClaimOwner owner,
                  std::uint32_t expectedRevision) const noexcept;
    void ReleaseClaim(Scope scope, std::uint64_t objectId,
                      std::uint32_t objectGeneration, std::uint32_t slot,
                      std::uint32_t paramIndex,
                      TargetClaimOwner owner) noexcept;
    bool SnapshotForClaim(Scope scope, std::uint64_t objectId,
                          std::uint32_t slot, std::uint32_t paramIndex,
                          TargetClaimOwner owner, float* resolved,
                          std::uint32_t* revision,
                          std::uint32_t* effectType) const noexcept;
    bool SnapshotForClaim(Scope scope, std::uint64_t objectId,
                          std::uint32_t objectGeneration,
                          std::uint32_t slot, std::uint32_t paramIndex,
                          TargetClaimOwner owner, float* resolved,
                          std::uint32_t* revision,
                          std::uint32_t* effectType) const noexcept;
    std::uint32_t TargetRevision(Scope scope, std::uint64_t objectId,
                                 std::uint32_t slot,
                                 std::uint32_t paramIndex) const noexcept;
    std::uint32_t TargetRevision(Scope scope, std::uint64_t objectId,
                                 std::uint32_t objectGeneration,
                                 std::uint32_t slot,
                                 std::uint32_t paramIndex) const noexcept;
    bool ExactTargetRevision(Scope scope, std::uint64_t objectId,
                             std::uint32_t slot, std::uint32_t paramIndex,
                             std::uint32_t expectedEffectType,
                             std::uint32_t* revision) const noexcept;
    bool ExactTargetRevision(Scope scope, std::uint64_t objectId,
                             std::uint32_t objectGeneration,
                             std::uint32_t slot, std::uint32_t paramIndex,
                             std::uint32_t expectedEffectType,
                             std::uint32_t* revision) const noexcept;
    bool ForeignClaim(Scope scope, std::uint64_t objectId,
                      std::uint32_t slot,
                      std::uint32_t paramIndex) const noexcept;
    bool ForeignClaim(Scope scope, std::uint64_t objectId,
                      std::uint32_t objectGeneration, std::uint32_t slot,
                      std::uint32_t paramIndex) const noexcept;
    bool ForeignClaimInSlot(Scope scope, std::uint64_t objectId,
                            std::uint32_t objectGeneration,
                            std::uint32_t slot) const noexcept;
    bool ClaimedInSlot(Scope scope, std::uint64_t objectId,
                       std::uint32_t objectGeneration,
                       std::uint32_t slot) const noexcept;
    void MarkSliceCommitted(Scope scope, std::uint64_t objectId,
                            std::uint32_t slot, std::uint32_t paramIndex,
                            TargetClaimOwner owner, float resolved) noexcept;
    void MarkSliceCommitted(Scope scope, std::uint64_t objectId,
                            std::uint32_t objectGeneration,
                            std::uint32_t slot, std::uint32_t paramIndex,
                            TargetClaimOwner owner, float resolved) noexcept;
    std::uint32_t RetiringRecordCountForTests() const noexcept;
    void SetNextRevisionForTests(std::uint64_t revision) noexcept {
        m_nextRevision = revision;
    }

    // Consumes a dirty value that cannot be represented by a stable render
    // command, preserving the last value accepted by the audio side.
    void MarkDropped(Scope scope, std::uint64_t objectId, std::uint32_t slot,
                     std::uint32_t paramIndex);

    bool BeginVariableProjection() noexcept;
    void RestoreVariableProjection() noexcept;
    std::uint64_t VariableProjectionStorageBytes() const noexcept;

private:
    struct VariableEvaluation {
        float contribution = 0.0f;
        float rebuildAccum = 0.0f;
        bool bindingActive = false;
        bool foldedThisSweep = false;
        bool dirty = false;
    };
    struct Record {
        Lifecycle lifecycle = Lifecycle::Free;
        Scope scope = Scope::Voice;
        std::uint64_t objectId = 0;
        std::uint32_t objectGeneration = 0;
        std::uint32_t slot = 0;
        std::uint32_t paramIndex = 0;
        std::uint32_t effectType = 0;
        EffectParamCombine combine = EffectParamCombine::None;
        float neutral = 0.0f;
        float clampMin = 0.0f;
        float clampMax = 0.0f; // kEffectParamClampNyquist => 0.5 * sampleRate
        float base = 0.0f;         // authored / last direct value
        float contribution = 0.0f; // folded binding accumulation (in slot form)
        float rebuildAccum = 0.0f; // in-progress fold during a sweep
        float lastCommitted = 0.0f;
        float meterProbeContribution = 0.0f;
        float meterProbeRebuild = 0.0f;
        bool meterProbeActive = false;
        bool meterProbeFoldedThisSweep = false;
        bool bindingActive = false;  // a binding folded a value last sweep
        bool foldedThisSweep = false; // reset by Begin, set by first Accumulate
        bool dirty = false;
        bool reserved = false;
        TargetClaimOwner preparedOwner{};
        std::uint32_t publicationRefCount = 0;
        std::uint32_t revision = 0;
    };

    Record* Find(Scope scope, std::uint64_t objectId, std::uint32_t slot,
                 std::uint32_t paramIndex);
    const Record* Find(Scope scope, std::uint64_t objectId, std::uint32_t slot,
                       std::uint32_t paramIndex) const;
    Record* FindExact(Scope scope, std::uint64_t objectId,
                      std::uint32_t objectGeneration, std::uint32_t slot,
                      std::uint32_t paramIndex, bool includeRetiring);
    const Record* FindExact(Scope scope, std::uint64_t objectId,
                            std::uint32_t objectGeneration,
                            std::uint32_t slot, std::uint32_t paramIndex,
                            bool includeRetiring) const;
    static std::uint32_t DefaultGeneration(Scope scope,
                                           std::uint64_t objectId) noexcept;
    bool SlotReserved(Scope scope, std::uint64_t objectId,
                      std::uint32_t objectGeneration,
                      std::uint32_t slot) const noexcept;
    bool AllocateRevision(std::uint32_t* out) noexcept;
    float ReduceRecord(const Record& r) const;
    float NeutralOf(EffectParamCombine combine) const {
        return combine == EffectParamCombine::Multiply ? 1.0f : 0.0f;
    }
    void SetDirty(Record& r) {
        if (!r.dirty) {
            ++m_dirtyCount;
            r.dirty = true;
        }
    }
    void CleanDirty(Record& r) {
        if (r.dirty) {
            --m_dirtyCount;
            r.dirty = false;
        }
    }

    std::uint32_t CountLive() const;

    std::uint32_t m_sampleRate = 0;
    std::uint32_t m_activeCapacity = 0;
    std::uint32_t m_totalCapacity = 0;
    std::vector<Record> m_records;
    std::vector<VariableEvaluation> m_projection;
    bool m_projectionActive = false;
    std::size_t m_projectionRecordCount = 0;
    std::uint32_t m_projectionDirtyCount = 0;
    std::uint64_t m_nextRevision = 1;
    std::uint32_t m_dirtyCount = 0;
    bool m_failSeedForTests = false;
    std::int32_t m_prepareSeedFailureAfterForTests = -1;
};

} // namespace harken

#endif // HARKEN_PARAM_EFFECT_PARAM_TARGET_STORE_H
