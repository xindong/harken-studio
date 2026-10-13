#include "harken/param/BusTargetStore.h"

#include <algorithm>
#include <cmath>
#include <exception>

namespace harken {

BusTargetStore::BusTargetStore(std::uint32_t maxBuses,
                               std::uint32_t retiringTargetCapacity)
    : m_maxBuses(maxBuses),
      m_activeGenerations(maxBuses, 0),
      m_retiringTargets(retiringTargetCapacity),
      m_meterProbeAccum(static_cast<std::size_t>(maxBuses) *
                            kBusTargetCount,
                        0.0f),
      m_meterProbeRampMs(static_cast<std::size_t>(maxBuses) *
                            kBusTargetCount,
                        0),
      m_meterProbeDepartureRampMs(
          static_cast<std::size_t>(maxBuses) * kBusTargetCount, 0),
      m_meterProbeSeen(static_cast<std::size_t>(maxBuses) *
                           kBusTargetCount,
                       0) {
    m_slots.assign(static_cast<std::size_t>(maxBuses) * kBusTargetCount *
                       kSlotCountPerTarget,
                   Slot{});
    m_publish.assign(static_cast<std::size_t>(maxBuses) * kBusTargetCount,
                     PublishState{});
    // Seed every changed-only reference to the target's neutral base: with
    // every slot inactive the reduction returns the base, so an untouched
    // bus target never publishes — the M5 no-binding identity.
    for (std::uint32_t bus = 0; bus < maxBuses; ++bus) {
        for (std::uint32_t t = 0; t < kBusTargetCount; ++t) {
            const TargetMeta* meta = FindTargetMeta(kTargetScopeBus, t);
            m_publish[PubIndexOf(bus, t)].lastCommitted =
                meta != nullptr ? meta->neutral : 0.0f;
        }
    }
}

bool BusTargetStore::InRange(std::uint32_t busIndex, std::uint32_t targetId,
                             std::uint32_t slot) const {
    return busIndex < m_maxBuses && targetId < kBusTargetCount &&
           slot < kSlotCountPerTarget;
}

std::size_t BusTargetStore::IndexOf(std::uint32_t busIndex,
                                    std::uint32_t targetId,
                                    std::uint32_t slot) const {
    return (static_cast<std::size_t>(busIndex) * kBusTargetCount + targetId) *
               kSlotCountPerTarget +
           slot;
}

void BusTargetStore::SetSlot(std::uint32_t busIndex, std::uint32_t targetId,
                             std::uint32_t slot, float domainValue) {
    if (!InRange(busIndex, targetId, slot)) {
        return;
    }
    Slot& s = m_slots[IndexOf(busIndex, targetId, slot)];
    s.value = domainValue;
    s.active = true;
}

void BusTargetStore::ClearSlot(std::uint32_t busIndex, std::uint32_t targetId,
                               std::uint32_t slot) {
    if (!InRange(busIndex, targetId, slot)) {
        return;
    }
    m_slots[IndexOf(busIndex, targetId, slot)] = Slot{};
}

void BusTargetStore::BeginMeterProbeSweep() noexcept {
    std::fill(m_meterProbeSeen.begin(), m_meterProbeSeen.end(), 0);
    std::fill(m_meterProbeRampMs.begin(), m_meterProbeRampMs.end(), 0);
}

void BusTargetStore::AccumulateMeterProbe(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, float contribution, bool changed,
    std::uint32_t transitionMs) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        m_activeGenerations[busIndex] != busGeneration) {
        return;
    }
    const TargetMeta* meta = FindTargetMeta(kTargetScopeBus, targetId);
    if (meta == nullptr) {
        return;
    }
    const std::size_t flat = PubIndexOf(busIndex, targetId);
    if (m_meterProbeSeen[flat] == 0) {
        m_meterProbeAccum[flat] = contribution;
        m_meterProbeSeen[flat] = 1;
    } else {
        m_meterProbeAccum[flat] =
            CombineValue(meta->domain, m_meterProbeAccum[flat], contribution);
    }
    if (changed) {
        m_meterProbeRampMs[flat] =
            std::max(m_meterProbeRampMs[flat], transitionMs);
    }
}

void BusTargetStore::NoteMeterProbeRemoved(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, std::uint32_t transitionMs) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        m_activeGenerations[busIndex] != busGeneration) {
        return;
    }
    const std::size_t flat = PubIndexOf(busIndex, targetId);
    m_meterProbeDepartureRampMs[flat] =
        std::max(m_meterProbeDepartureRampMs[flat], transitionMs);
}

void BusTargetStore::EndMeterProbeSweep() {
    for (std::uint32_t busIndex = 0; busIndex < m_maxBuses; ++busIndex) {
        if (m_activeGenerations[busIndex] == 0) {
            continue;
        }
        for (std::uint32_t targetId = 0; targetId < kBusTargetCount;
             ++targetId) {
            const std::size_t flat = PubIndexOf(busIndex, targetId);
            Slot& slot =
                m_slots[IndexOf(busIndex, targetId, kSlotMeterProbeIndex)];
            const bool seen = m_meterProbeSeen[flat] != 0;
            const bool aggregateChanged =
                slot.active != seen ||
                (seen && slot.value != m_meterProbeAccum[flat]);
            if (aggregateChanged) {
                bool apply = true;
                if (seen) {
                    const float prospective = ReduceWithSlot(
                        busIndex, targetId, kSlotMeterProbeIndex,
                        m_meterProbeAccum[flat]);
                    if (std::isfinite(prospective)) {
                        slot.value = m_meterProbeAccum[flat];
                        slot.active = true;
                    } else {
                        apply = false;
                        m_meterProbeAccum[flat] =
                            slot.active ? slot.value : 0.0f;
                    }
                } else {
                    slot = Slot{};
                }
                if (apply) {
                    MarkVariableChanged(
                        busIndex, targetId,
                        std::max(m_meterProbeRampMs[flat],
                                 m_meterProbeDepartureRampMs[flat]));
                }
            }
            m_meterProbeDepartureRampMs[flat] = 0;
        }
    }
}

bool BusTargetStore::SlotActive(std::uint32_t busIndex, std::uint32_t targetId,
                                std::uint32_t slot) const {
    if (!InRange(busIndex, targetId, slot)) {
        return false;
    }
    return m_slots[IndexOf(busIndex, targetId, slot)].active;
}

float BusTargetStore::Reduce(std::uint32_t busIndex,
                             std::uint32_t targetId) const {
    const TargetMeta* meta = FindTargetMeta(kTargetScopeBus, targetId);
    if (meta == nullptr || busIndex >= m_maxBuses) {
        return 0.0f;
    }
    float accumulated = meta->neutral;
    for (std::uint32_t slot = 0; slot < kSlotCountPerTarget; ++slot) {
        const Slot& s = m_slots[IndexOf(busIndex, targetId, slot)];
        if (s.active) {
            accumulated = CombineValue(meta->domain, accumulated, s.value);
        }
    }
    if (meta->clamped) {
        accumulated = accumulated < meta->clampMin ? meta->clampMin
                      : accumulated > meta->clampMax ? meta->clampMax
                                                     : accumulated;
    }
    return accumulated;
}

float BusTargetStore::ReduceWithSlot(std::uint32_t busIndex,
                                     std::uint32_t targetId,
                                     std::uint32_t slot,
                                     float domainValue) const {
    const TargetMeta* meta = FindTargetMeta(kTargetScopeBus, targetId);
    if (meta == nullptr || !InRange(busIndex, targetId, slot)) {
        return 0.0f;
    }
    float accumulated = meta->neutral;
    for (std::uint32_t candidate = 0; candidate < kSlotCountPerTarget;
         ++candidate) {
        if (candidate == slot) {
            accumulated =
                CombineValue(meta->domain, accumulated, domainValue);
            continue;
        }
        const Slot& s = m_slots[IndexOf(busIndex, targetId, candidate)];
        if (s.active) {
            accumulated = CombineValue(meta->domain, accumulated, s.value);
        }
    }
    if (meta->clamped) {
        accumulated = accumulated < meta->clampMin ? meta->clampMin
                      : accumulated > meta->clampMax ? meta->clampMax
                                                     : accumulated;
    }
    return accumulated;
}

void BusTargetStore::ResetActiveRows(std::uint32_t busIndex) noexcept {
    if (busIndex >= m_maxBuses) {
        return;
    }
    for (std::uint32_t t = 0; t < kBusTargetCount; ++t) {
        for (std::uint32_t slot = 0; slot < kSlotCountPerTarget; ++slot) {
            m_slots[IndexOf(busIndex, t, slot)] = Slot{};
        }
        PublishState& state = m_publish[PubIndexOf(busIndex, t)];
        if (state.dirty || state.pendingInput) {
            --m_demandCount;
        }
        if (state.pendingInput && m_pendingInputMarks > 0) {
            --m_pendingInputMarks;
        }
        state = PublishState{};
        const TargetMeta* meta = FindTargetMeta(kTargetScopeBus, t);
        state.lastCommitted = meta != nullptr ? meta->neutral : 0.0f;
        const std::size_t flat = PubIndexOf(busIndex, t);
        m_meterProbeAccum[flat] = 0.0f;
        m_meterProbeRampMs[flat] = 0;
        m_meterProbeDepartureRampMs[flat] = 0;
        m_meterProbeSeen[flat] = 0;
    }
}

void BusTargetStore::ResetBus(std::uint32_t busIndex) {
    if (busIndex >= m_maxBuses) {
        return;
    }
    RetireBusGeneration(busIndex, m_activeGenerations[busIndex]);
    ResetActiveRows(busIndex);
}

void BusTargetStore::RetireBusGeneration(
    std::uint32_t busIndex, std::uint32_t busGeneration) noexcept {
    if (busIndex >= m_maxBuses ||
        m_activeGenerations[busIndex] != busGeneration) {
        return;
    }
    for (std::uint32_t targetId = 0; targetId < kBusTargetCount;
         ++targetId) {
        PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
        if (!state.preparedOwner.Valid()) {
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
        retiring->busIndex = busIndex;
        retiring->busGeneration = busGeneration;
        retiring->targetId = targetId;
        retiring->owner = state.preparedOwner;
        retiring->revision = state.revision;
        retiring->frozenResolved = Reduce(busIndex, targetId);
        retiring->frozenRampMs = state.claimSnapshotCaptured
                                    ? state.preparedRampMs
                                    : state.pendingRampMs;
        retiring->claimSnapshotCaptured =
            state.claimSnapshotCaptured;
        state.suppressClaimRamp = false;
        state.claimSnapshotCaptured = false;
        state.preCandidatePendingRampMs = 0;
        state.preCandidateApiRampMs = 0;
        state.preCandidateApiRampTick = 0;
        state.candidateBaselineValid = false;
        state.preparedOwner = {};
    }
    ResetActiveRows(busIndex);
    m_activeGenerations[busIndex] = 0;
}

void BusTargetStore::InitializeBusGeneration(
    std::uint32_t busIndex, std::uint32_t busGeneration) noexcept {
    if (busIndex >= m_maxBuses) {
        return;
    }
    if (m_activeGenerations[busIndex] != 0 &&
        m_activeGenerations[busIndex] != busGeneration) {
        RetireBusGeneration(busIndex, m_activeGenerations[busIndex]);
    }
    ResetActiveRows(busIndex);
    m_activeGenerations[busIndex] = busGeneration;
}

std::uint32_t BusTargetStore::ActiveGeneration(
    std::uint32_t busIndex) const noexcept {
    return busIndex < m_activeGenerations.size()
               ? m_activeGenerations[busIndex]
               : 0;
}

void BusTargetStore::MarkVariableChanged(std::uint32_t busIndex,
                                         std::uint32_t targetId,
                                         std::uint32_t transitionMs) {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return;
    }
    PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
    SetDirty(state);
    if (!state.suppressClaimRamp &&
        transitionMs > state.pendingRampMs) {
        state.pendingRampMs = transitionMs;
    }
}

void BusTargetStore::MarkPendingInput(std::uint32_t busIndex,
                                      std::uint32_t targetId) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return;
    }
    PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
    if (state.pendingInput) {
        return;
    }
    if (!state.dirty) {
        ++m_demandCount;
    }
    state.pendingInput = true;
    ++m_pendingInputMarks;
}

void BusTargetStore::ConsumePendingInput() {
    if (m_pendingInputMarks == 0) {
        return; // guarded sweep: zero marks, zero work
    }
    for (PublishState& state : m_publish) {
        if (!state.pendingInput) {
            continue;
        }
        if (!state.dirty) {
            --m_demandCount;
        }
        state.pendingInput = false;
    }
    m_pendingInputMarks = 0;
}

void BusTargetStore::NoteApiDirect(std::uint32_t busIndex,
                                   std::uint32_t targetId, float domainValue,
                                   std::uint32_t rampMs,
                                   float committedResolved) {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return;
    }
    Slot& api = m_slots[IndexOf(busIndex, targetId, kSlotApiDirectIndex)];
    api.value = domainValue;
    api.active = true;
    PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
    // The direct setter committed the reduced value in the same call
    // (section 10.3), so the flush must not re-publish it: the reference
    // updates and the plain dirty mark cleans. A pending-input mark stays —
    // its demand belongs to the next evaluation, not to this setter — and
    // the pending binding-ramp aggregate stays with it (the voice-side
    // SetApiDirect discipline). Several same-tick setters fold their
    // rampMs by max, order-free like the rest of the aggregation.
    state.lastCommitted = committedResolved;
    CleanDirty(state);
    // The direct setter's own command carried this rampMs — record it for
    // attribution capture like any committed publication.
    state.lastPublishedRampMs = rampMs;
    if (state.apiRampTick == m_flushTick) {
        if (rampMs > state.apiRampMs) {
            state.apiRampMs = rampMs;
        }
    } else {
        state.apiRampMs = rampMs;
        state.apiRampTick = m_flushTick;
    }
}

void BusTargetStore::MarkCommitted(std::uint32_t busIndex,
                                   std::uint32_t targetId, float resolved,
                                   std::uint32_t rampMs) {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return;
    }
    PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
    state.lastCommitted = resolved;
    CleanDirty(state);
    state.pendingRampMs = 0;
    state.lastPublishedRampMs = rampMs;
}

void BusTargetStore::MarkDropped(std::uint32_t busIndex,
                                 std::uint32_t targetId) {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return;
    }
    // Consumed without a command: clean dirty so the value never
    // re-presents on its own, but leave lastCommitted and the published
    // ramp on the last audio-accepted value — no command existed.
    PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
    CleanDirty(state);
    state.pendingRampMs = 0;
}

bool BusTargetStore::TryClaim(std::uint32_t busIndex,
                              std::uint32_t targetId,
                              VariableSliceRef owner,
                              std::uint32_t expectedRevision) noexcept {
    return TryClaim(busIndex, ActiveGeneration(busIndex), targetId, owner,
                    expectedRevision);
}

bool BusTargetStore::TryClaim(std::uint32_t busIndex,
                              std::uint32_t busGeneration,
                              std::uint32_t targetId,
                              VariableSliceRef owner,
                              std::uint32_t expectedRevision) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        !owner.Valid() || m_activeGenerations[busIndex] != busGeneration) {
        return false;
    }
    PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
    if (state.revision != expectedRevision || state.preparedOwner.Valid()) {
        return false;
    }
    state.preparedOwner = owner;
    state.preparedRampMs = 0;
    state.suppressClaimRamp = false;
    state.claimSnapshotCaptured = false;
    state.preCandidatePendingRampMs = 0;
    state.preCandidateApiRampMs = 0;
    state.preCandidateApiRampTick = 0;
    state.candidateBaselineValid = false;
    return true;
}

bool BusTargetStore::CaptureCandidateBaseline(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, VariableSliceRef owner) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        !owner.Valid() || m_activeGenerations[busIndex] != busGeneration) {
        return false;
    }
    PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
    if (state.preparedOwner.slot != owner.slot ||
        state.preparedOwner.generation != owner.generation ||
        state.preparedRampMs != 0 || state.claimSnapshotCaptured) {
        return false;
    }
    if (!state.candidateBaselineValid) {
        state.preCandidatePendingRampMs = state.pendingRampMs;
        state.preCandidateApiRampMs = state.apiRampMs;
        state.preCandidateApiRampTick = state.apiRampTick;
        state.candidateBaselineValid = true;
    }
    return true;
}

void BusTargetStore::ClearCandidateBaseline(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, VariableSliceRef owner) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        m_activeGenerations[busIndex] != busGeneration) {
        return;
    }
    PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
    if (state.preparedOwner.slot != owner.slot ||
        state.preparedOwner.generation != owner.generation) {
        return;
    }
    state.preCandidatePendingRampMs = 0;
    state.preCandidateApiRampMs = 0;
    state.preCandidateApiRampTick = 0;
    state.candidateBaselineValid = false;
}

void BusTargetStore::DropClaimedPublication(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, VariableSliceRef owner) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        m_activeGenerations[busIndex] != busGeneration) {
        return;
    }
    PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
    if (state.preparedOwner.slot != owner.slot ||
        state.preparedOwner.generation != owner.generation) {
        return;
    }
    MarkDropped(busIndex, targetId);
    state.preparedRampMs = 0;
    state.suppressClaimRamp = false;
    state.claimSnapshotCaptured = false;
    state.preCandidatePendingRampMs = 0;
    state.preCandidateApiRampMs = 0;
    state.preCandidateApiRampTick = 0;
    state.candidateBaselineValid = false;
    state.preparedOwner = {};
}

void BusTargetStore::ReleaseClaim(std::uint32_t busIndex,
                                  std::uint32_t targetId,
                                  VariableSliceRef owner,
                                  ClaimReleaseReason reason) noexcept {
    ReleaseClaim(busIndex, ActiveGeneration(busIndex), targetId, owner,
                 reason);
}

void BusTargetStore::ReleaseClaim(std::uint32_t busIndex,
                                  std::uint32_t busGeneration,
                                  std::uint32_t targetId,
                                  VariableSliceRef owner,
                                  ClaimReleaseReason reason) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return;
    }
    if (m_activeGenerations[busIndex] == busGeneration) {
        PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
        if (state.preparedOwner.slot == owner.slot &&
            state.preparedOwner.generation == owner.generation) {
            if (reason == ClaimReleaseReason::RejectCandidate) {
                if (state.candidateBaselineValid) {
                    state.pendingRampMs = state.preCandidatePendingRampMs;
                    state.apiRampMs = state.preCandidateApiRampMs;
                    state.apiRampTick = state.preCandidateApiRampTick;
                }
            } else if (state.preparedRampMs > state.pendingRampMs) {
                state.pendingRampMs = state.preparedRampMs;
            }
            state.preparedRampMs = 0;
            state.suppressClaimRamp = false;
            state.claimSnapshotCaptured = false;
            state.preCandidatePendingRampMs = 0;
            state.preCandidateApiRampMs = 0;
            state.preCandidateApiRampTick = 0;
            state.candidateBaselineValid = false;
            state.preparedOwner = {};
            return;
        }
    }
    if (RetiringTarget* retiring =
            FindRetiring(busIndex, busGeneration, targetId, owner)) {
        *retiring = RetiringTarget{};
    }
}

bool BusTargetStore::SnapshotForClaim(
    std::uint32_t busIndex, std::uint32_t targetId, VariableSliceRef owner,
    float* resolved, std::uint32_t* rampMs,
    std::uint32_t* revision) const noexcept {
    return SnapshotForClaim(busIndex, ActiveGeneration(busIndex), targetId,
                            owner, resolved, rampMs, revision);
}

bool BusTargetStore::SnapshotForClaim(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, VariableSliceRef owner, float* resolved,
    std::uint32_t* rampMs, std::uint32_t* revision) const noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        resolved == nullptr || rampMs == nullptr || revision == nullptr) {
        return false;
    }
    if (m_activeGenerations[busIndex] == busGeneration) {
        const PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
        if (state.preparedOwner.slot == owner.slot &&
            state.preparedOwner.generation == owner.generation) {
            *resolved = Reduce(busIndex, targetId);
            *rampMs = state.pendingRampMs;
            *revision = state.revision;
            return true;
        }
    }
    const RetiringTarget* retiring =
        FindRetiring(busIndex, busGeneration, targetId, owner);
    if (retiring == nullptr) {
        return false;
    }
    *resolved = retiring->frozenResolved;
    *rampMs = retiring->frozenRampMs;
    *revision = retiring->revision;
    return true;
}

bool BusTargetStore::CaptureSnapshotForClaim(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, VariableSliceRef owner, float* resolved,
    std::uint32_t* rampMs, std::uint32_t* revision) noexcept {
    if (resolved == nullptr || rampMs == nullptr || revision == nullptr) {
        return false;
    }
    if (busIndex < m_maxBuses && targetId < kBusTargetCount &&
        m_activeGenerations[busIndex] == busGeneration) {
        PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
        if (state.preparedOwner.slot == owner.slot &&
            state.preparedOwner.generation == owner.generation) {
            if (state.claimSnapshotCaptured) {
                return false;
            }
            *resolved = Reduce(busIndex, targetId);
            *rampMs = state.pendingRampMs;
            *revision = state.revision;
            state.preparedRampMs = state.pendingRampMs;
            state.pendingRampMs = 0;
            state.claimSnapshotCaptured = true;
            return true;
        }
    }
    RetiringTarget* retiring =
        FindRetiring(busIndex, busGeneration, targetId, owner);
    if (retiring == nullptr || retiring->claimSnapshotCaptured) {
        return false;
    }
    *resolved = retiring->frozenResolved;
    *rampMs = retiring->frozenRampMs;
    *revision = retiring->revision;
    retiring->claimSnapshotCaptured = true;
    return true;
}

void BusTargetStore::SetClaimRampSuppressed(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, VariableSliceRef owner,
    bool suppressed) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        m_activeGenerations[busIndex] != busGeneration) {
        return;
    }
    PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
    if (state.preparedOwner.slot == owner.slot &&
        state.preparedOwner.generation == owner.generation) {
        state.suppressClaimRamp = suppressed;
    }
}

std::uint32_t BusTargetStore::TargetRevision(
    std::uint32_t busIndex, std::uint32_t targetId) const noexcept {
    return TargetRevision(busIndex, ActiveGeneration(busIndex), targetId);
}

std::uint32_t BusTargetStore::TargetRevision(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId) const noexcept {
    if (busIndex < m_maxBuses && targetId < kBusTargetCount &&
        m_activeGenerations[busIndex] == busGeneration) {
        return m_publish[PubIndexOf(busIndex, targetId)].revision;
    }
    for (const RetiringTarget& retiring : m_retiringTargets) {
        if (retiring.used && retiring.busIndex == busIndex &&
            retiring.busGeneration == busGeneration &&
            retiring.targetId == targetId) {
            return retiring.revision;
        }
    }
    return 0;
}

bool BusTargetStore::ForeignClaim(std::uint32_t busIndex,
                                  std::uint32_t targetId) const noexcept {
    return ForeignClaim(busIndex, ActiveGeneration(busIndex), targetId);
}

bool BusTargetStore::ForeignClaim(std::uint32_t busIndex,
                                  std::uint32_t busGeneration,
                                  std::uint32_t targetId) const noexcept {
    if (busIndex < m_maxBuses && targetId < kBusTargetCount &&
        m_activeGenerations[busIndex] == busGeneration &&
        m_publish[PubIndexOf(busIndex, targetId)].preparedOwner.Valid()) {
        return true;
    }
    for (const RetiringTarget& retiring : m_retiringTargets) {
        if (retiring.used && retiring.busIndex == busIndex &&
            retiring.busGeneration == busGeneration &&
            retiring.targetId == targetId) {
            return true;
        }
    }
    return false;
}

void BusTargetStore::MarkSliceCommitted(
    std::uint32_t busIndex, std::uint32_t targetId, VariableSliceRef owner,
    float resolved, std::uint32_t rampMs) noexcept {
    MarkSliceCommitted(busIndex, ActiveGeneration(busIndex), targetId, owner,
                       resolved, rampMs);
}

void BusTargetStore::MarkSliceCommitted(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, VariableSliceRef owner, float resolved,
    std::uint32_t rampMs) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return;
    }
    if (m_activeGenerations[busIndex] == busGeneration) {
        PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
        if (state.preparedOwner.slot == owner.slot &&
            state.preparedOwner.generation == owner.generation) {
            state.lastCommitted = resolved;
            state.lastPublishedRampMs = rampMs;
            if (Reduce(busIndex, targetId) != resolved) {
                SetDirty(state);
            } else {
                state.pendingRampMs = 0;
                CleanDirty(state);
            }
            state.preparedRampMs = 0;
            state.suppressClaimRamp = false;
            state.claimSnapshotCaptured = false;
            state.preCandidatePendingRampMs = 0;
            state.preCandidateApiRampMs = 0;
            state.preCandidateApiRampTick = 0;
            state.candidateBaselineValid = false;
            state.preparedOwner = {};
            return;
        }
    }
    if (RetiringTarget* retiring =
            FindRetiring(busIndex, busGeneration, targetId, owner)) {
        *retiring = RetiringTarget{};
    }
}

BusTargetStore::RetiringTarget* BusTargetStore::FindRetiring(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, VariableSliceRef owner) noexcept {
    return const_cast<RetiringTarget*>(
        static_cast<const BusTargetStore*>(this)->FindRetiring(
            busIndex, busGeneration, targetId, owner));
}

const BusTargetStore::RetiringTarget* BusTargetStore::FindRetiring(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, VariableSliceRef owner) const noexcept {
    for (const RetiringTarget& retiring : m_retiringTargets) {
        if (retiring.used && retiring.busIndex == busIndex &&
            retiring.busGeneration == busGeneration &&
            retiring.targetId == targetId &&
            retiring.owner.slot == owner.slot &&
            retiring.owner.generation == owner.generation) {
            return &retiring;
        }
    }
    return nullptr;
}

std::uint32_t BusTargetStore::RetiringTargetCountForTests() const noexcept {
    std::uint32_t count = 0;
    for (const RetiringTarget& retiring : m_retiringTargets) {
        count += retiring.used ? 1u : 0u;
    }
    return count;
}

float BusTargetStore::LastCommitted(std::uint32_t busIndex,
                                    std::uint32_t targetId) const {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return 0.0f;
    }
    return m_publish[PubIndexOf(busIndex, targetId)].lastCommitted;
}

float BusTargetStore::SlotValue(std::uint32_t busIndex,
                                std::uint32_t targetId,
                                std::uint32_t slot) const {
    if (!InRange(busIndex, targetId, slot)) {
        return 0.0f;
    }
    const Slot& s = m_slots[IndexOf(busIndex, targetId, slot)];
    return s.active ? s.value : 0.0f;
}

bool BusTargetStore::PendingPublication(std::uint32_t busIndex,
                                        std::uint32_t targetId) const {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return false;
    }
    const PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
    return state.dirty || state.pendingInput;
}

std::uint32_t
BusTargetStore::LastPublishedRampMs(std::uint32_t busIndex,
                                    std::uint32_t targetId) const {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return 0;
    }
    return m_publish[PubIndexOf(busIndex, targetId)].lastPublishedRampMs;
}

std::uint32_t BusTargetStore::PendingRampMs(std::uint32_t busIndex,
                                            std::uint32_t targetId) const {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return 0;
    }
    const PublishState& state = m_publish[PubIndexOf(busIndex, targetId)];
    std::uint32_t rampMs = state.pendingRampMs;
    if (state.apiRampTick == m_flushTick && state.apiRampMs > rampMs) {
        rampMs = state.apiRampMs;
    }
    return rampMs;
}

} // namespace harken
