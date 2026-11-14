#include "harken/param/BusTargetStore.h"
#include "harken/param/BindingTable.h"
#include <cassert>

#include <algorithm>
#include <cmath>
#include <exception>
#include <new>
#include <utility>

namespace harken {

BusTargetStore::BusTargetStore(std::uint32_t maxBuses,
                               std::uint32_t retiringTargetCapacity)
    : m_maxBuses(maxBuses), m_settings(static_cast<std::size_t>(maxBuses) * kBusTargetCount),
      m_activeGenerations(maxBuses, 0) {
    if (!m_retiringTargets.Reserve(retiringTargetCapacity)) throw std::bad_alloc{};
    for (std::uint32_t bus = 0; bus < maxBuses; ++bus)
        for (std::uint32_t t = 0; t < kBusTargetCount; ++t) {
            const auto* meta = FindTargetMeta(kTargetScopeBus, t);
            m_settings[PubIndexOf(bus, t)].lastCommitted = meta ? meta->neutral : 0;
        }
}

BusTargetStore::BusTargetStore(const BusTargetStore& other)
    : m_maxBuses(other.m_maxBuses), m_settings(other.m_settings),
      m_projectionActive(other.m_projectionActive),
      m_projectionDemandCount(other.m_projectionDemandCount),
      m_projectionPendingInputMarks(other.m_projectionPendingInputMarks),
      m_activeGenerations(other.m_activeGenerations), m_demandCount(other.m_demandCount),
      m_variableSlotCount(other.m_variableSlotCount), m_pendingInputMarks(other.m_pendingInputMarks),
      m_flushTick(other.m_flushTick), m_flushCursor(other.m_flushCursor) {
    for (auto& setting : m_settings) setting.activity = nullptr;
    m_activities.reserve(other.m_activities.size());
    for (const auto& activity : other.m_activities) {
        auto copy = std::make_unique<Activity>(*activity);
        m_settings[copy->flat].activity = copy.get();
        m_activities.push_back(std::move(copy));
    }
    if (!m_retiringTargets.Reserve(other.m_retiringTargets.Capacity()) ||
        !m_retiringTargets.ConstructThrough(other.m_retiringTargets.ConstructedCount())) throw std::bad_alloc{};
    std::copy(other.m_retiringTargets.begin(), other.m_retiringTargets.end(), m_retiringTargets.begin());
}

bool BusTargetStore::PrepareBusCapacity(std::uint32_t count) noexcept {
    if (count <= m_maxBuses) return true;
    if (m_projectionActive) return false;
    try {
        const auto rows = static_cast<std::size_t>(count) * kBusTargetCount;
        m_settings.reserve(rows); m_settings.resize(rows);
        m_activeGenerations.reserve(count); m_activeGenerations.resize(count);
        for (auto bus = m_maxBuses; bus < count; ++bus)
            for (std::uint32_t t = 0; t < kBusTargetCount; ++t) {
                const auto* meta = FindTargetMeta(kTargetScopeBus, t);
                m_settings[PubIndexOf(bus, t)].lastCommitted = meta ? meta->neutral : 0;
            }
        m_maxBuses = count;
        return true;
    } catch (...) { return false; }
}

bool BusTargetStore::PrepareActivity(std::uint32_t bus, std::uint32_t target) noexcept {
    if (!InRange(bus, target, 0)) return false;
    auto& settings = m_settings[PubIndexOf(bus, target)];
    if (settings.activity) return true;
    if (m_projectionActive) return false;
    try {
        auto activity = std::make_unique<Activity>();
        activity->flat = PubIndexOf(bus, target);
        m_activities.push_back(std::move(activity));
        settings.activity = m_activities.back().get();
        return true;
    } catch (...) { return false; }
}

BusTargetStore::Activity* BusTargetStore::FindActivity(std::uint32_t bus, std::uint32_t target) noexcept {
    return InRange(bus, target, 0) ? m_settings[PubIndexOf(bus, target)].activity : nullptr;
}

const BusTargetStore::PublishState& BusTargetStore::Publication(std::uint32_t bus,
    std::uint32_t target) const noexcept {
    static const PublishState empty;
    const auto* activity = m_settings[PubIndexOf(bus, target)].activity;
    return activity ? activity->publish : empty;
}

BusTargetStore::Slot BusTargetStore::ReadSlot(std::uint32_t bus, std::uint32_t target,
    std::uint32_t slot) const noexcept {
    const auto& setting = m_settings[PubIndexOf(bus, target)];
    if (slot < kSlotVariableIndex)
        return {setting.values[slot], (setting.activeMask & (1u << slot)) != 0};
    if (!setting.activity) return {};
    return slot == kSlotVariableIndex ? setting.activity->variable : setting.activity->meter;
}

void BusTargetStore::ReleaseUnusedActivities(BindingTable& bindings) noexcept {
    if (m_projectionActive || m_activities.empty()) return;
    for (auto& activity : m_activities) activity->attached = false;
    const auto mark = [&](const auto& attachment, const auto& definition) {
        if (attachment.scopeKind != kTargetScopeBus ||
            (attachment.targetGeneration != 0 &&
             attachment.targetGeneration != ActiveGeneration(attachment.busIndex))) return;
        if (auto* activity = FindActivity(attachment.busIndex, definition.targetId)) activity->attached = true;
    };
    bindings.ForEachBusAttachment(mark);
    bindings.ForEachProbeAttachment(mark);
    // A callback can detach after evaluation but before this service cleanup.
    // Even an unset contribution leaves a dirty retired dependency until the
    // next fold. Its no-fail variable notification still needs this backing.
    BindingDirtyTarget pending;
    auto& dependencies = bindings.Dependencies();
    for (std::uint32_t cursor = 0; dependencies.DirtyTargetAt(cursor, &pending); ++cursor) {
        BindingDependencyIdentity identity;
        if (!dependencies.TargetIdentity(pending, &identity) ||
            identity.scope != kTargetScopeBus ||
            identity.detail != static_cast<std::uint32_t>(BindingSourceKind::Variable)) continue;
        const auto bus = static_cast<std::uint32_t>(identity.object);
        if (identity.generation != 0 && identity.generation != ActiveGeneration(bus)) continue;
        if (auto* activity = FindActivity(bus, identity.target)) activity->attached = true;
    }
    for (std::size_t i = 0; i < m_activities.size();) {
        const auto& activity = *m_activities[i];
        const auto& state = activity.publish;
        if (activity.attached || activity.variable.active || activity.meter.active ||
            state.dirty || state.pendingInput || state.pendingRampMs != 0 ||
            state.preparedOwner.Valid() || state.publicationRefCount != 0 ||
            state.preparedRampMs != 0 || state.candidateBaselineValid ||
            state.claimSnapshotCaptured || state.suppressClaimRamp || activity.meterDepartureRampMs != 0) {
            ++i; continue;
        }
        m_settings[activity.flat].activity = nullptr;
        if (i + 1 != m_activities.size()) m_activities[i] = std::move(m_activities.back());
        m_activities.pop_back();
    }
}

std::uint64_t BusTargetStore::StorageBytes() const noexcept {
    return m_settings.capacity() * sizeof(Settings) + m_activities.capacity() * sizeof(std::unique_ptr<Activity>) +
        m_activities.size() * sizeof(Activity) + m_activeGenerations.capacity() * sizeof(std::uint32_t) +
        m_retiringTargets.CommittedBytes();
}

bool BusTargetStore::BeginVariableProjection() noexcept {
    if (m_projectionActive) return false;
    for (auto& activity : m_activities) {
        const auto& state = activity->publish;
        activity->projection = {activity->variable, state.pendingRampMs,
            state.dirty, state.pendingInput, state.suppressClaimRamp};
    }
    m_projectionDemandCount = m_demandCount;
    m_projectionPendingInputMarks = m_pendingInputMarks;
    m_projectionActive = true;
    return true;
}

std::uint32_t BusTargetStore::TakePublicationRamp(std::uint32_t busIndex,
    std::uint32_t busGeneration, std::uint32_t targetId,
    TargetClaimOwner owner) noexcept {
    if (!owner.Shared() || !InRange(busIndex, targetId, 0) ||
        ActiveGeneration(busIndex) != busGeneration) return 0;
    auto* activity = FindActivity(busIndex, targetId);
    if (!activity) return 0;
    auto& state = activity->publish;
    if (state.preparedOwner != owner) return 0;
    return std::exchange(state.pendingRampMs, 0u);
}

void BusTargetStore::RestorePublicationRamp(std::uint32_t busIndex,
    std::uint32_t busGeneration, std::uint32_t targetId,
    TargetClaimOwner owner, std::uint32_t rampMs) noexcept {
    if (!owner.Shared() || !InRange(busIndex, targetId, 0) ||
        ActiveGeneration(busIndex) != busGeneration || rampMs == 0) return;
    auto* activity = FindActivity(busIndex, targetId);
    if (!activity) return;
    auto& state = activity->publish;
    if (state.preparedOwner != owner) return;
    state.pendingRampMs = std::max(state.pendingRampMs, rampMs);
    SetDirty(state);
}

void BusTargetStore::RestoreVariableProjection() noexcept {
    if (!m_projectionActive) return;
    for (auto& activity : m_activities) {
        auto& state = activity->publish;
        const auto& saved = activity->projection;
        m_variableSlotCount -= activity->variable.active;
        activity->variable = saved.variable;
        m_variableSlotCount += activity->variable.active;
        state.pendingRampMs = saved.pendingRampMs;
        state.dirty = saved.dirty;
        state.pendingInput = saved.pendingInput;
        state.suppressClaimRamp = saved.suppressClaimRamp;
    }
    m_demandCount = m_projectionDemandCount;
    m_pendingInputMarks = m_projectionPendingInputMarks;
    m_projectionActive = false;
}

std::uint64_t BusTargetStore::VariableProjectionStorageBytes() const noexcept {
    return m_activities.size() * sizeof(VariableEvaluation);
}

bool BusTargetStore::InRange(std::uint32_t busIndex, std::uint32_t targetId,
                             std::uint32_t slot) const {
    return busIndex < m_maxBuses && targetId < kBusTargetCount &&
           slot < kSlotCountPerTarget;
}

void BusTargetStore::SetSlot(std::uint32_t busIndex, std::uint32_t targetId,
                             std::uint32_t slot, double domainValue) {
    if (!InRange(busIndex, targetId, slot)) return;
    auto& settings = m_settings[PubIndexOf(busIndex, targetId)];
    if (slot < kSlotVariableIndex) {
        settings.values[slot] = domainValue;
        settings.activeMask |= 1u << slot;
        return;
    }
    if (!PrepareActivity(busIndex, targetId)) throw std::bad_alloc{};
    auto& current = slot == kSlotVariableIndex ? settings.activity->variable : settings.activity->meter;
    if (!current.active && slot == kSlotVariableIndex) ++m_variableSlotCount;
    current = {domainValue, true};
}

void BusTargetStore::ClearSlot(std::uint32_t busIndex, std::uint32_t targetId,
                               std::uint32_t slot) {
    if (!InRange(busIndex, targetId, slot)) return;
    auto& settings = m_settings[PubIndexOf(busIndex, targetId)];
    if (slot < kSlotVariableIndex) {
        settings.values[slot] = 0;
        settings.activeMask &= ~(1u << slot);
        return;
    }
    if (!settings.activity) return;
    auto& current = slot == kSlotVariableIndex ? settings.activity->variable : settings.activity->meter;
    if (current.active && slot == kSlotVariableIndex) --m_variableSlotCount;
    current = {};
}

void BusTargetStore::BeginMeterProbeSweep() noexcept {
    for (auto& activity : m_activities) {
        activity->meterSeen = false;
        activity->meterRampMs = 0;
    }
}

void BusTargetStore::AccumulateMeterProbe(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, float contribution, bool changed,
    std::uint32_t transitionMs) noexcept {
    if (!InRange(busIndex, targetId, 0) || ActiveGeneration(busIndex) != busGeneration) return;
    const auto* meta = FindTargetMeta(kTargetScopeBus, targetId);
    if (!meta) return;
    auto* activity = FindActivity(busIndex, targetId);
    assert(activity != nullptr);
    if (!activity) std::terminate();
    activity->meterAccum = activity->meterSeen
        ? CombineValue(meta->domain, activity->meterAccum, contribution) : contribution;
    activity->meterSeen = true;
    if (changed) activity->meterRampMs = std::max(activity->meterRampMs, transitionMs);
}

void BusTargetStore::NoteMeterProbeRemoved(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, std::uint32_t transitionMs) noexcept {
    if (!InRange(busIndex, targetId, 0) || ActiveGeneration(busIndex) != busGeneration) return;
    auto* activity = FindActivity(busIndex, targetId);
    assert(activity != nullptr);
    if (!activity) std::terminate();
    activity->meterDepartureRampMs = std::max(activity->meterDepartureRampMs, transitionMs);
}

void BusTargetStore::EndMeterProbeSweep() {
    for (auto& entry : m_activities) {
        auto& activity = *entry;
        const auto busIndex = static_cast<std::uint32_t>(activity.flat / kBusTargetCount);
        const auto targetId = static_cast<std::uint32_t>(activity.flat % kBusTargetCount);
        if (m_activeGenerations[busIndex] == 0) continue;
        Slot& slot = activity.meter;
        const bool seen = activity.meterSeen;
            const bool aggregateChanged =
                slot.active != seen ||
                (seen && slot.value != activity.meterAccum);
            if (aggregateChanged) {
                bool apply = true;
                if (seen) {
                    const float prospective = ReduceWithSlot(
                        busIndex, targetId, kSlotMeterProbeIndex,
                        activity.meterAccum);
                    if (std::isfinite(prospective)) {
                        slot.value = activity.meterAccum;
                        slot.active = true;
                    } else {
                        apply = false;
                        activity.meterAccum =
                            slot.active ? slot.value : 0.0f;
                    }
                } else {
                    slot = Slot{};
                }
                if (apply) {
                    MarkVariableChanged(
                        busIndex, targetId,
                        std::max(activity.meterRampMs,
                                 activity.meterDepartureRampMs));
                }
            }
            activity.meterDepartureRampMs = 0;
    }
}

bool BusTargetStore::SlotActive(std::uint32_t busIndex, std::uint32_t targetId,
                                std::uint32_t slot) const {
    if (!InRange(busIndex, targetId, slot)) {
        return false;
    }
    return ReadSlot(busIndex, targetId, slot).active;
}

float BusTargetStore::Reduce(std::uint32_t busIndex,
                             std::uint32_t targetId) const {
    const TargetMeta* meta = FindTargetMeta(kTargetScopeBus, targetId);
    if (meta == nullptr || busIndex >= m_maxBuses) {
        return 0.0f;
    }
    double accumulated = meta->neutral;
    for (std::uint32_t slot = 0; slot < kSlotCountPerTarget; ++slot) {
        const Slot& s = ReadSlot(busIndex, targetId, slot);
        if (s.active) {
            accumulated = CombineValue(meta->domain, accumulated, s.value);
        }
    }
    if (meta->clamped) {
        accumulated = accumulated < meta->clampMin ? meta->clampMin
                      : accumulated > meta->clampMax ? meta->clampMax
                                                     : accumulated;
    }
    return NarrowResolvedValue(accumulated);
}

float BusTargetStore::ReduceWithSlot(std::uint32_t busIndex,
                                     std::uint32_t targetId,
                                     std::uint32_t slot,
                                     double domainValue) const {
    const TargetMeta* meta = FindTargetMeta(kTargetScopeBus, targetId);
    if (meta == nullptr || !InRange(busIndex, targetId, slot)) {
        return 0.0f;
    }
    double accumulated = meta->neutral;
    for (std::uint32_t candidate = 0; candidate < kSlotCountPerTarget;
         ++candidate) {
        if (candidate == slot) {
            accumulated =
                CombineValue(meta->domain, accumulated, domainValue);
            continue;
        }
        const Slot& s = ReadSlot(busIndex, targetId, candidate);
        if (s.active) {
            accumulated = CombineValue(meta->domain, accumulated, s.value);
        }
    }
    if (meta->clamped) {
        accumulated = accumulated < meta->clampMin ? meta->clampMin
                      : accumulated > meta->clampMax ? meta->clampMax
                                                     : accumulated;
    }
    return NarrowResolvedValue(accumulated);
}

void BusTargetStore::ResetActiveRows(std::uint32_t busIndex) noexcept {
    if (busIndex >= m_maxBuses) return;
    for (std::uint32_t t = 0; t < kBusTargetCount; ++t) {
        auto& settings = m_settings[PubIndexOf(busIndex, t)];
        auto* activity = settings.activity;
        if (activity) {
            if (activity->publish.dirty || activity->publish.pendingInput) --m_demandCount;
            if (activity->publish.pendingInput) --m_pendingInputMarks;
            m_variableSlotCount -= activity->variable.active;
            const auto flat = activity->flat;
            *activity = {};
            activity->flat = flat;
        }
        settings = {};
        settings.activity = activity;
        const auto* meta = FindTargetMeta(kTargetScopeBus, t);
        settings.lastCommitted = meta ? meta->neutral : 0;
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
        auto* activity = FindActivity(busIndex, targetId);
        if (!activity) continue;
        auto& state = activity->publish;
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
        retiring->publicationRefCount = state.publicationRefCount;
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
        state.publicationRefCount = 0;
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
    if (!PrepareActivity(busIndex, targetId)) throw std::bad_alloc{};
    auto& state = FindActivity(busIndex, targetId)->publish;
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
    if (!FindActivity(busIndex, targetId)) std::terminate();
    auto& state = FindActivity(busIndex, targetId)->publish;
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
    for (auto& activity : m_activities) {
        auto& state = activity->publish;
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
    if (!InRange(busIndex, targetId, 0)) return;
    SetSlot(busIndex, targetId, kSlotApiDirectIndex, domainValue);
    auto& settings = m_settings[PubIndexOf(busIndex, targetId)];
    settings.lastCommitted = committedResolved;
    if (settings.activity) CleanDirty(settings.activity->publish);
    settings.lastPublishedRampMs = rampMs;
    if (settings.apiRampTick == m_flushTick) settings.apiRampMs = std::max(settings.apiRampMs, rampMs);
    else { settings.apiRampMs = rampMs; settings.apiRampTick = m_flushTick; }
}

void BusTargetStore::MarkCommitted(std::uint32_t busIndex,
                                   std::uint32_t targetId, float resolved,
                                   std::uint32_t rampMs) {
    if (!InRange(busIndex, targetId, 0)) return;
    auto& settings = m_settings[PubIndexOf(busIndex, targetId)];
    settings.lastCommitted = resolved;
    settings.lastPublishedRampMs = rampMs;
    if (settings.activity) {
        CleanDirty(settings.activity->publish);
        settings.activity->publish.pendingRampMs = 0;
    }
}

void BusTargetStore::MarkDropped(std::uint32_t busIndex,
                                 std::uint32_t targetId) {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return;
    }
    // Consumed without a command: clean dirty so the value never
    // re-presents on its own, but leave lastCommitted and the published
    // ramp on the last audio-accepted value — no command existed.
    auto* activity = FindActivity(busIndex, targetId);
    if (!activity) return;
    auto& state = activity->publish;
    CleanDirty(state);
    state.pendingRampMs = 0;
}

bool BusTargetStore::TryClaim(std::uint32_t busIndex,
                              std::uint32_t targetId,
                              TargetClaimOwner owner,
                              std::uint32_t expectedRevision) noexcept {
    return TryClaim(busIndex, ActiveGeneration(busIndex), targetId, owner,
                    expectedRevision);
}

bool BusTargetStore::TryClaim(std::uint32_t busIndex,
                              std::uint32_t busGeneration,
                              std::uint32_t targetId,
                              TargetClaimOwner owner,
                              std::uint32_t expectedRevision) noexcept {
    if (!CanClaim(busIndex, busGeneration, targetId, owner, expectedRevision) ||
        !PrepareActivity(busIndex, targetId) || !PrepareRetiringTargetStorage()) return false;
    PublishState& state = FindActivity(busIndex, targetId)->publish;
    if (state.preparedOwner.Valid()) {
        ++state.publicationRefCount;
        return true;
    }
    state.preparedOwner = owner;
    state.publicationRefCount = owner.Shared() ? 1u : 0u;
    state.preparedRampMs = 0;
    state.suppressClaimRamp = false;
    state.claimSnapshotCaptured = false;
    state.preCandidatePendingRampMs = 0;
    state.preCandidateApiRampMs = 0;
    state.preCandidateApiRampTick = 0;
    state.candidateBaselineValid = false;
    return true;
}

bool BusTargetStore::CanClaim(std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, TargetClaimOwner owner, std::uint32_t expectedRevision) const noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount || !owner.Valid() ||
        m_activeGenerations[busIndex] != busGeneration) return false;
    const auto& state = Publication(busIndex, targetId);
    if (state.revision != expectedRevision) return false;
    return state.preparedOwner.Valid()
        ? owner.Shared() && state.preparedOwner == owner && state.publicationRefCount != UINT32_MAX
        : state.publicationRefCount == 0;
}

bool BusTargetStore::CaptureCandidateBaseline(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, TargetClaimOwner owner) noexcept {
    if (owner.Shared()) return false;
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        !owner.Valid() || m_activeGenerations[busIndex] != busGeneration) {
        return false;
    }
    auto* activity = FindActivity(busIndex, targetId);
    if (!activity) return false;
    auto& state = activity->publish;
    if (state.preparedOwner != owner ||
        state.preparedRampMs != 0 || state.claimSnapshotCaptured) {
        return false;
    }
    if (!state.candidateBaselineValid) {
        state.preCandidatePendingRampMs = state.pendingRampMs;
        state.preCandidateApiRampMs = m_settings[PubIndexOf(busIndex, targetId)].apiRampMs;
        state.preCandidateApiRampTick = m_settings[PubIndexOf(busIndex, targetId)].apiRampTick;
        state.candidateBaselineValid = true;
    }
    return true;
}

void BusTargetStore::ClearCandidateBaseline(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, TargetClaimOwner owner) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        m_activeGenerations[busIndex] != busGeneration) {
        return;
    }
    auto* activity = FindActivity(busIndex, targetId);
    if (!activity) return;
    auto& state = activity->publish;
    if (state.preparedOwner != owner) {
        return;
    }
    state.preCandidatePendingRampMs = 0;
    state.preCandidateApiRampMs = 0;
    state.preCandidateApiRampTick = 0;
    state.candidateBaselineValid = false;
}

void BusTargetStore::DropClaimedPublication(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, TargetClaimOwner owner) noexcept {
    if (owner.Shared()) return;
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        m_activeGenerations[busIndex] != busGeneration) {
        return;
    }
    auto* activity = FindActivity(busIndex, targetId);
    if (!activity) return;
    auto& state = activity->publish;
    if (state.preparedOwner != owner) {
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

void BusTargetStore::DropClaimedBaseline(std::uint32_t busIndex,
                                        std::uint32_t busGeneration,
                                        std::uint32_t targetId,
                                        TargetClaimOwner owner) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        m_activeGenerations[busIndex] != busGeneration) return;
    auto* activity = FindActivity(busIndex, targetId);
    if (!activity) return;
    auto& state = activity->publish;
    if (state.preparedOwner != owner) return;
    MarkDropped(busIndex, targetId);
    state.preCandidatePendingRampMs = 0;
}

void BusTargetStore::ReleaseClaim(std::uint32_t busIndex,
                                  std::uint32_t targetId,
                                  TargetClaimOwner owner,
                                  ClaimReleaseReason reason) noexcept {
    ReleaseClaim(busIndex, ActiveGeneration(busIndex), targetId, owner,
                 reason);
}

void BusTargetStore::ReleaseClaim(std::uint32_t busIndex,
                                  std::uint32_t busGeneration,
                                  std::uint32_t targetId,
                                  TargetClaimOwner owner,
                                  ClaimReleaseReason reason) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return;
    }
    if (m_activeGenerations[busIndex] == busGeneration && FindActivity(busIndex, targetId)) {
        auto& state = FindActivity(busIndex, targetId)->publish;
        if (state.preparedOwner == owner) {
            if (owner.Shared()) {
                if (state.publicationRefCount == 0 || --state.publicationRefCount != 0) return;
                state.preparedOwner = {};
                state.suppressClaimRamp = false;
                return;
            }
            if (reason == ClaimReleaseReason::RejectCandidate) {
                if (state.candidateBaselineValid) {
                    state.pendingRampMs = state.preCandidatePendingRampMs;
                    m_settings[PubIndexOf(busIndex, targetId)].apiRampMs = state.preCandidateApiRampMs;
                    m_settings[PubIndexOf(busIndex, targetId)].apiRampTick = state.preCandidateApiRampTick;
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
        if (owner.Shared() && (retiring->publicationRefCount == 0 ||
            --retiring->publicationRefCount != 0)) return;
        *retiring = RetiringTarget{};
    }
}

bool BusTargetStore::SnapshotForClaim(
    std::uint32_t busIndex, std::uint32_t targetId, TargetClaimOwner owner,
    float* resolved, std::uint32_t* rampMs,
    std::uint32_t* revision) const noexcept {
    return SnapshotForClaim(busIndex, ActiveGeneration(busIndex), targetId,
                            owner, resolved, rampMs, revision);
}

bool BusTargetStore::SnapshotForClaim(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, TargetClaimOwner owner, float* resolved,
    std::uint32_t* rampMs, std::uint32_t* revision) const noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        resolved == nullptr || rampMs == nullptr || revision == nullptr) {
        return false;
    }
    if (m_activeGenerations[busIndex] == busGeneration) {
        const PublishState& state = Publication(busIndex, targetId);
        if (state.preparedOwner == owner) {
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
    std::uint32_t targetId, TargetClaimOwner owner, float* resolved,
    std::uint32_t* rampMs, std::uint32_t* revision) noexcept {
    if (resolved == nullptr || rampMs == nullptr || revision == nullptr) {
        return false;
    }
    if (busIndex < m_maxBuses && targetId < kBusTargetCount &&
        m_activeGenerations[busIndex] == busGeneration && FindActivity(busIndex, targetId)) {
        auto& state = FindActivity(busIndex, targetId)->publish;
        if (state.preparedOwner == owner) {
            if (owner.Shared()) {
                *resolved = Reduce(busIndex, targetId);
                *rampMs = state.pendingRampMs;
                *revision = state.revision;
                return true;
            }
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
    if (retiring == nullptr || (!owner.Shared() && retiring->claimSnapshotCaptured)) {
        return false;
    }
    *resolved = retiring->frozenResolved;
    *rampMs = retiring->frozenRampMs;
    *revision = retiring->revision;
    if (!owner.Shared()) retiring->claimSnapshotCaptured = true;
    return true;
}

void BusTargetStore::SetClaimRampSuppressed(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, TargetClaimOwner owner,
    bool suppressed) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount ||
        m_activeGenerations[busIndex] != busGeneration) {
        return;
    }
    auto* activity = FindActivity(busIndex, targetId);
    if (!activity) return;
    auto& state = activity->publish;
    if (state.preparedOwner == owner) {
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
        return Publication(busIndex, targetId).revision;
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
        Publication(busIndex, targetId).preparedOwner.Valid()) {
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
    std::uint32_t busIndex, std::uint32_t targetId, TargetClaimOwner owner,
    float resolved, std::uint32_t rampMs) noexcept {
    MarkSliceCommitted(busIndex, ActiveGeneration(busIndex), targetId, owner,
                       resolved, rampMs);
}

void BusTargetStore::MarkSliceCommitted(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, TargetClaimOwner owner, float resolved,
    std::uint32_t rampMs) noexcept {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return;
    }
    if (m_activeGenerations[busIndex] == busGeneration && FindActivity(busIndex, targetId)) {
        auto& state = FindActivity(busIndex, targetId)->publish;
        if (state.preparedOwner == owner) {
            m_settings[PubIndexOf(busIndex, targetId)].lastCommitted = resolved;
            m_settings[PubIndexOf(busIndex, targetId)].lastPublishedRampMs = rampMs;
            if (owner.Shared()) {
                if (Reduce(busIndex, targetId) != resolved || state.pendingInput || state.pendingRampMs != 0)
                    SetDirty(state);
                else
                    CleanDirty(state);
                ReleaseClaim(busIndex, busGeneration, targetId, owner);
                return;
            }
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
        if (owner.Shared() && (retiring->publicationRefCount == 0 ||
            --retiring->publicationRefCount != 0)) return;
        *retiring = RetiringTarget{};
    }
}

BusTargetStore::RetiringTarget* BusTargetStore::FindRetiring(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, TargetClaimOwner owner) noexcept {
    return const_cast<RetiringTarget*>(
        static_cast<const BusTargetStore*>(this)->FindRetiring(
            busIndex, busGeneration, targetId, owner));
}

const BusTargetStore::RetiringTarget* BusTargetStore::FindRetiring(
    std::uint32_t busIndex, std::uint32_t busGeneration,
    std::uint32_t targetId, TargetClaimOwner owner) const noexcept {
    for (const RetiringTarget& retiring : m_retiringTargets) {
        if (retiring.used && retiring.busIndex == busIndex &&
            retiring.busGeneration == busGeneration &&
            retiring.targetId == targetId &&
            retiring.owner == owner) {
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
    return m_settings[PubIndexOf(busIndex, targetId)].lastCommitted;
}

float BusTargetStore::SlotValue(std::uint32_t busIndex,
                                std::uint32_t targetId,
                                std::uint32_t slot) const {
    if (!InRange(busIndex, targetId, slot)) {
        return 0.0f;
    }
    const Slot& s = ReadSlot(busIndex, targetId, slot);
    return s.active ? NarrowResolvedValue(s.value) : 0.0f;
}

bool BusTargetStore::PendingPublication(std::uint32_t busIndex,
                                        std::uint32_t targetId) const {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return false;
    }
    const PublishState& state = Publication(busIndex, targetId);
    return state.dirty || state.pendingInput;
}

std::uint32_t
BusTargetStore::LastPublishedRampMs(std::uint32_t busIndex,
                                    std::uint32_t targetId) const {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return 0;
    }
    return m_settings[PubIndexOf(busIndex, targetId)].lastPublishedRampMs;
}

std::uint32_t BusTargetStore::PendingRampMs(std::uint32_t busIndex,
                                            std::uint32_t targetId) const {
    if (busIndex >= m_maxBuses || targetId >= kBusTargetCount) {
        return 0;
    }
    const PublishState& state = Publication(busIndex, targetId);
    std::uint32_t rampMs = state.pendingRampMs;
    if (m_settings[PubIndexOf(busIndex, targetId)].apiRampTick == m_flushTick && m_settings[PubIndexOf(busIndex, targetId)].apiRampMs > rampMs) {
        rampMs = m_settings[PubIndexOf(busIndex, targetId)].apiRampMs;
    }
    return rampMs;
}

} // namespace harken
