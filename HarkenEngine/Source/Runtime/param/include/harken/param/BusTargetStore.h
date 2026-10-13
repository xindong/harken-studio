// Bus-scope contribution storage: pack-authored mix, API direct, public
// Variable bindings, and private MeterProbe bindings reduce in that order
// onto each target's neutral base. Multiple bindings of one source kind
// accumulate in-domain into that kind's slot before it is written here; the
// store holds each slot's reduced accumulation, not individual bindings.
//
// The store also owns the bus targets' publication bookkeeping — the
// per-target mirror of the voice-side TargetState: the last committed
// resolved value (the changed-only reference; it starts at the neutral
// base, so an untouched bus publishes nothing and the no-binding path
// stays bit-identical to M5), the dirty / pending-input demand marks whose
// union feeds the admission reservation's live census (section 10.2), and
// the section-9.2 ramp aggregation state (the max transitionMs folded by
// changed binding contributions since the target last published, plus a
// tick-stamped API-direct rampMs).
//
// Storage is maxBuses x kBusTargetCount x kSlotCountPerTarget slots plus
// one publication record per (bus, target), preallocated at construction;
// no allocation and no locks after init. Logic thread only.
#ifndef HARKEN_PARAM_BUS_TARGET_STORE_H
#define HARKEN_PARAM_BUS_TARGET_STORE_H

#include "harken/core/VariableSliceTable.h"
#include "harken/param/ParamTargetTable.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace harken {

class BusTargetStore {
public:
    // Slot indices, reduced in this order. The order is part of the
    // model's determinism guarantee. Authored is pack-owned; the remaining
    // three match the per-voice combine-slot order.
    enum : std::uint32_t {
        kSlotAuthoredIndex = 0,
        kSlotApiDirectIndex = 1,
        kSlotVariableIndex = 2,
        kSlotMeterProbeIndex = 3,
        kSlotCountPerTarget = 4
    };

    explicit BusTargetStore(std::uint32_t maxBuses,
                            std::uint32_t retiringTargetCapacity = 0);

    // Writes / clears one slot of one bus target, addressed by dense bus
    // index. Out-of-range indices are ignored (the callers own bus
    // validation; the store never invents state for unknown addresses).
    void SetSlot(std::uint32_t busIndex, std::uint32_t targetId,
                 std::uint32_t slot, float domainValue);
    void ClearSlot(std::uint32_t busIndex, std::uint32_t targetId,
                   std::uint32_t slot);

    void BeginMeterProbeSweep() noexcept;
    void AccumulateMeterProbe(std::uint32_t busIndex,
                              std::uint32_t busGeneration,
                              std::uint32_t targetId, float contribution,
                              bool changed,
                              std::uint32_t transitionMs) noexcept;
    void NoteMeterProbeRemoved(std::uint32_t busIndex,
                               std::uint32_t busGeneration,
                               std::uint32_t targetId,
                               std::uint32_t transitionMs) noexcept;
    void EndMeterProbeSweep();

    bool SlotActive(std::uint32_t busIndex, std::uint32_t targetId,
                    std::uint32_t slot) const;

    // Reduces one bus target: the metadata table's neutral base, then
    // authored, API-direct, Variable, and MeterProbe, folded with the
    // target's declared domain. Untouched slots contribute nothing; an
    // unknown address reduces to 0.
    float Reduce(std::uint32_t busIndex, std::uint32_t targetId) const;

    // Same fold as Reduce, but one slot is substituted before folding.
    // Inactive stored slots still contribute nothing; the override is
    // always treated as active. Unknown addresses reduce to 0.
    float ReduceWithSlot(std::uint32_t busIndex, std::uint32_t targetId,
                         std::uint32_t slot, float domainValue) const;

    std::uint32_t MaxBuses() const { return m_maxBuses; }

    // --- publication bookkeeping (sections 9.2 / 10) ---

    // Marks a bus target dirty because its Variable-slot accumulation
    // changed this tick, folding `transitionMs` of each changed
    // contributor into the pending ramp aggregate (max; it survives a
    // deferred publication until the commit lands).
    void MarkVariableChanged(std::uint32_t busIndex, std::uint32_t targetId,
                             std::uint32_t transitionMs);

    // Marks a bus target as pending-input: a Variable write is known to
    // move a binding on it before the next evaluation realizes the change.
    // Counts toward the demand census like dirty; only the evaluation pass
    // consumes it (ConsumePendingInput below), so a demand known since a
    // deferral survives until the gather that realizes it.
    void MarkPendingInput(std::uint32_t busIndex,
                          std::uint32_t targetId) noexcept;
    void ConsumePendingInput();

    // Records an API-direct change (Vn_SetBusVolume): retargets the
    // API-direct slot, updates the changed-only reference (the direct
    // setter publishes its own reduced command in the same call, section
    // 10.3), cleans the plain dirty state that publish consumed, and
    // stamps rampMs for the tick's section-9.2 aggregation window.
    void NoteApiDirect(std::uint32_t busIndex, std::uint32_t targetId,
                       float domainValue, std::uint32_t rampMs,
                       float committedResolved);

    // Union census of dirty and pending-input marks — the live demand the
    // admission reservation reads during a deferral (a target counts once
    // whether dirty, pending-input, or both).
    std::uint32_t DemandCount() const { return m_demandCount; }

    // Flush walk: visits every dirty (bus, target) with its resolved value
    // and the section-9.2 aggregated rampMs (pending ramp folded with a
    // same-tick API-direct rampMs). fn returns true when the target entered
    // the current batch. The caller invokes MarkCommitted / MarkDropped per
    // handled target; a target left untouched stays
    // dirty and re-presents next tick (the deferred-batch retry
    // discipline). A target whose reduction equals the last committed
    // value cleans quietly and publishes nothing.
    template <typename Fn>
    void FlushDirty(Fn&& fn) {
        const std::size_t targetCount =
            static_cast<std::size_t>(m_maxBuses) * kBusTargetCount;
        std::size_t nextCursor = m_flushCursor;
        bool stagedAny = false;
        for (std::size_t offset = 0; offset < targetCount; ++offset) {
            const std::size_t flat = (m_flushCursor + offset) % targetCount;
            const std::uint32_t bus =
                static_cast<std::uint32_t>(flat / kBusTargetCount);
            const std::uint32_t t =
                static_cast<std::uint32_t>(flat % kBusTargetCount);
            PublishState& state = m_publish[PubIndexOf(bus, t)];
            if (!state.dirty) {
                continue;
            }
            if (state.preparedOwner.Valid()) {
                continue;
            }
            const float resolved = Reduce(bus, t);
            if (resolved == state.lastCommitted) {
                CleanDirty(state);
                state.pendingRampMs = 0;
                continue;
            }
            std::uint32_t rampMs = state.pendingRampMs;
            if (state.apiRampTick == m_flushTick &&
                state.apiRampMs > rampMs) {
                rampMs = state.apiRampMs;
            }
            state.pendingRampMs = rampMs;
            if (fn(bus, t, resolved, rampMs)) {
                stagedAny = true;
                nextCursor = (flat + 1) % targetCount;
            }
        }
        if (stagedAny) {
            m_flushCursor = nextCursor;
        }
        // API-direct ramp stamps expire with the tick window, mirroring
        // the voice-side flush-tick discipline.
        ++m_flushTick;
    }

    // Outcome of one flushed commit. MarkCommitted: a command reached the
    // queue — the value becomes the changed-only reference, the target
    // cleans, and rampMs (the ramp the command carried) becomes the
    // attribution record. MarkDropped: consumed without a command
    // (poisoned value) — dirty cleans so the value never re-presents on
    // its own; lastCommitted stays the last value accepted by the audio
    // side, and the attribution record keeps the previous published ramp.
    // A deferred (staged-only) target gets neither call and stays dirty
    // with its ramp aggregate intact.
    void MarkCommitted(std::uint32_t busIndex, std::uint32_t targetId,
                       float resolved, std::uint32_t rampMs = 0);
    void MarkDropped(std::uint32_t busIndex, std::uint32_t targetId);

    float LastCommitted(std::uint32_t busIndex, std::uint32_t targetId) const;
    bool TryClaim(std::uint32_t busIndex, std::uint32_t targetId,
                  VariableSliceRef owner, std::uint32_t expectedRevision) noexcept;
    bool TryClaim(std::uint32_t busIndex, std::uint32_t busGeneration,
                  std::uint32_t targetId, VariableSliceRef owner,
                  std::uint32_t expectedRevision) noexcept;
    bool CaptureCandidateBaseline(std::uint32_t busIndex,
                                  std::uint32_t busGeneration,
                                  std::uint32_t targetId,
                                  VariableSliceRef owner) noexcept;
    void ClearCandidateBaseline(std::uint32_t busIndex,
                                std::uint32_t busGeneration,
                                std::uint32_t targetId,
                                VariableSliceRef owner) noexcept;
    void DropClaimedPublication(std::uint32_t busIndex,
                                std::uint32_t busGeneration,
                                std::uint32_t targetId,
                                VariableSliceRef owner) noexcept;
    void ReleaseClaim(std::uint32_t busIndex, std::uint32_t targetId,
                      VariableSliceRef owner,
                      ClaimReleaseReason reason =
                          ClaimReleaseReason::RetryPreserve) noexcept;
    void ReleaseClaim(std::uint32_t busIndex, std::uint32_t busGeneration,
                      std::uint32_t targetId, VariableSliceRef owner,
                      ClaimReleaseReason reason =
                          ClaimReleaseReason::RetryPreserve) noexcept;
    std::uint32_t PendingRampMs(std::uint32_t busIndex,
                                std::uint32_t targetId) const;
    bool SnapshotForClaim(std::uint32_t busIndex, std::uint32_t targetId,
                          VariableSliceRef owner, float* resolved,
                          std::uint32_t* rampMs,
                          std::uint32_t* revision) const noexcept;
    bool SnapshotForClaim(std::uint32_t busIndex,
                          std::uint32_t busGeneration,
                          std::uint32_t targetId, VariableSliceRef owner,
                          float* resolved, std::uint32_t* rampMs,
                          std::uint32_t* revision) const noexcept;
    bool CaptureSnapshotForClaim(
        std::uint32_t busIndex, std::uint32_t busGeneration,
        std::uint32_t targetId, VariableSliceRef owner, float* resolved,
        std::uint32_t* rampMs, std::uint32_t* revision) noexcept;
    void SetClaimRampSuppressed(std::uint32_t busIndex,
                                std::uint32_t busGeneration,
                                std::uint32_t targetId,
                                VariableSliceRef owner,
                                bool suppressed) noexcept;
    std::uint32_t TargetRevision(std::uint32_t busIndex,
                                 std::uint32_t targetId) const noexcept;
    std::uint32_t TargetRevision(std::uint32_t busIndex,
                                 std::uint32_t busGeneration,
                                 std::uint32_t targetId) const noexcept;
    bool ForeignClaim(std::uint32_t busIndex,
                      std::uint32_t targetId) const noexcept;
    bool ForeignClaim(std::uint32_t busIndex, std::uint32_t busGeneration,
                      std::uint32_t targetId) const noexcept;
    void MarkSliceCommitted(std::uint32_t busIndex, std::uint32_t targetId,
                            VariableSliceRef owner, float resolved,
                            std::uint32_t rampMs) noexcept;
    void MarkSliceCommitted(std::uint32_t busIndex,
                            std::uint32_t busGeneration,
                            std::uint32_t targetId, VariableSliceRef owner,
                            float resolved, std::uint32_t rampMs) noexcept;

    void RetireBusGeneration(std::uint32_t busIndex,
                             std::uint32_t busGeneration) noexcept;
    void InitializeBusGeneration(std::uint32_t busIndex,
                                 std::uint32_t busGeneration) noexcept;
    std::uint32_t ActiveGeneration(std::uint32_t busIndex) const noexcept;
    std::uint32_t RetiringTargetCountForTests() const noexcept;

    // Returns one bus's rows to their initial state: every combine slot
    // inactive and the publication record re-seeded at the neutral base with
    // its demand marks released (the census stays exact). The destroy path's
    // cleanup — a tombstoned bus must neither re-present a dirty value nor
    // hold admission demand.
    void ResetBus(std::uint32_t busIndex);

    // --- attribution capture accessors (section 12; logic thread) ---

    // One slot's stored domain value (0 when inactive or out of range).
    float SlotValue(std::uint32_t busIndex, std::uint32_t targetId,
                    std::uint32_t slot) const;

    // Whether a publication demand mark (dirty or pending-input) is set.
    bool PendingPublication(std::uint32_t busIndex,
                            std::uint32_t targetId) const;

    // The rampMs the target's most recent committed command carried
    // (direct setters included; 0 before any).
    std::uint32_t LastPublishedRampMs(std::uint32_t busIndex,
                                      std::uint32_t targetId) const;

private:
    struct Slot {
        float value = 0.0f;
        bool active = false;
    };

    // Per (bus, target) publication record — the voice TargetState mirror.
    struct PublishState {
        float lastCommitted = 0.0f; // seeded to the neutral base at init
        bool dirty = false;
        bool pendingInput = false;
        bool suppressClaimRamp = false;
        bool claimSnapshotCaptured = false;
        std::uint32_t pendingRampMs = 0;
        std::uint32_t apiRampMs = 0;
        std::uint32_t preparedRampMs = 0;
        std::uint64_t apiRampTick = 0;
        std::uint32_t preCandidatePendingRampMs = 0;
        std::uint32_t preCandidateApiRampMs = 0;
        std::uint64_t preCandidateApiRampTick = 0;
        bool candidateBaselineValid = false;
        // The rampMs the most recent committed command carried; the
        // attribution capture's record, nothing else reads it.
        std::uint32_t lastPublishedRampMs = 0;
        std::uint32_t revision = 1;
        VariableSliceRef preparedOwner{};
    };

    struct RetiringTarget {
        bool used = false;
        bool claimSnapshotCaptured = false;
        std::uint32_t busIndex = 0;
        std::uint32_t busGeneration = 0;
        std::uint32_t targetId = 0;
        VariableSliceRef owner{};
        std::uint32_t revision = 0;
        float frozenResolved = 0.0f;
        std::uint32_t frozenRampMs = 0;
    };

    bool InRange(std::uint32_t busIndex, std::uint32_t targetId,
                 std::uint32_t slot) const;
    std::size_t IndexOf(std::uint32_t busIndex, std::uint32_t targetId,
                        std::uint32_t slot) const;
    std::size_t PubIndexOf(std::uint32_t busIndex,
                           std::uint32_t targetId) const {
        return static_cast<std::size_t>(busIndex) * kBusTargetCount +
               targetId;
    }
    // Demand-census transitions, mirroring the voice-side helpers: every
    // dirty / pending-input flip funnels through these so m_demandCount
    // stays an exact union census.
    void SetDirty(PublishState& state) {
        if (!state.dirty && !state.pendingInput) {
            ++m_demandCount;
        }
        state.dirty = true;
    }
    void CleanDirty(PublishState& state) {
        if (state.dirty && !state.pendingInput) {
            --m_demandCount;
        }
        state.dirty = false;
    }
    void ResetActiveRows(std::uint32_t busIndex) noexcept;
    RetiringTarget* FindRetiring(std::uint32_t busIndex,
                                 std::uint32_t busGeneration,
                                 std::uint32_t targetId,
                                 VariableSliceRef owner) noexcept;
    const RetiringTarget* FindRetiring(
        std::uint32_t busIndex, std::uint32_t busGeneration,
        std::uint32_t targetId, VariableSliceRef owner) const noexcept;

    std::uint32_t m_maxBuses = 0;
    std::vector<Slot> m_slots;           // fixed size after construction
    std::vector<PublishState> m_publish; // one per (bus, target)
    std::vector<std::uint32_t> m_activeGenerations;
    std::vector<RetiringTarget> m_retiringTargets;
    std::vector<float> m_meterProbeAccum;
    std::vector<std::uint32_t> m_meterProbeRampMs;
    std::vector<std::uint32_t> m_meterProbeDepartureRampMs;
    std::vector<std::uint8_t> m_meterProbeSeen;
    std::uint32_t m_demandCount = 0;
    std::uint32_t m_pendingInputMarks = 0;
    // Starts above the PublishState default so a never-stamped target can
    // never alias the first tick (the voice-side m_flushTick precedent).
    std::uint64_t m_flushTick = 1;
    // Fixed-capacity round-robin start for over-capacity dirty sets.
    std::size_t m_flushCursor = 0;
};

} // namespace harken

#endif // HARKEN_PARAM_BUS_TARGET_STORE_H
