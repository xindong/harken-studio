#include "harken/param/EffectParamTargetStore.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <exception>
#include <limits>

namespace harken {

EffectParamTargetStore::EffectParamTargetStore(std::uint32_t sampleRate,
                                               std::uint32_t capacity,
                                               std::uint32_t retiringCapacity)
    : m_sampleRate(sampleRate), m_activeCapacity(capacity),
      m_totalCapacity(capacity + retiringCapacity) {
    m_records.reserve(m_totalCapacity);
    m_projection.resize(m_totalCapacity);
}

bool EffectParamTargetStore::BeginVariableProjection() noexcept {
    if (m_projectionActive) return false;
    m_projectionRecordCount = m_records.size();
    m_projectionDirtyCount = m_dirtyCount;
    for (std::size_t i = 0; i < m_records.size(); ++i) {
        const auto& record = m_records[i];
        m_projection[i] = {record.contribution, record.rebuildAccum,
            record.bindingActive, record.foldedThisSweep, record.dirty};
    }
    m_projectionActive = true;
    return true;
}

void EffectParamTargetStore::RestoreVariableProjection() noexcept {
    if (!m_projectionActive) return;
    assert(m_records.size() == m_projectionRecordCount);
    for (std::size_t i = 0; i < m_records.size(); ++i) {
        auto& record = m_records[i];
        const auto& saved = m_projection[i];
        record.contribution = saved.contribution;
        record.rebuildAccum = saved.rebuildAccum;
        record.bindingActive = saved.bindingActive;
        record.foldedThisSweep = saved.foldedThisSweep;
        record.dirty = saved.dirty;
    }
    m_dirtyCount = m_projectionDirtyCount;
    m_projectionActive = false;
}

std::uint64_t EffectParamTargetStore::VariableProjectionStorageBytes() const noexcept {
    return m_projection.size() * sizeof(VariableEvaluation);
}

std::uint32_t EffectParamTargetStore::DefaultGeneration(
    Scope scope, std::uint64_t objectId) noexcept {
    return scope == Scope::Voice ? static_cast<std::uint32_t>(objectId >> 32)
                                 : 0;
}

EffectParamTargetStore::Record*
EffectParamTargetStore::Find(Scope scope, std::uint64_t objectId,
                             std::uint32_t slot, std::uint32_t paramIndex) {
    for (Record& r : m_records) {
        if (r.lifecycle == Lifecycle::Live && r.scope == scope &&
            r.objectId == objectId &&
            r.slot == slot && r.paramIndex == paramIndex) {
            return &r;
        }
    }
    return nullptr;
}

const EffectParamTargetStore::Record*
EffectParamTargetStore::Find(Scope scope, std::uint64_t objectId,
                             std::uint32_t slot,
                             std::uint32_t paramIndex) const {
    for (const Record& r : m_records) {
        if (r.lifecycle == Lifecycle::Live && r.scope == scope &&
            r.objectId == objectId &&
            r.slot == slot && r.paramIndex == paramIndex) {
            return &r;
        }
    }
    return nullptr;
}

EffectParamTargetStore::Record* EffectParamTargetStore::FindExact(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t paramIndex, bool includeRetiring) {
    return const_cast<Record*>(
        static_cast<const EffectParamTargetStore*>(this)->FindExact(
            scope, objectId, objectGeneration, slot, paramIndex,
            includeRetiring));
}

const EffectParamTargetStore::Record* EffectParamTargetStore::FindExact(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t paramIndex,
    bool includeRetiring) const {
    for (const Record& record : m_records) {
        if (record.scope == scope && record.objectId == objectId &&
            record.objectGeneration == objectGeneration &&
            record.slot == slot && record.paramIndex == paramIndex &&
            (record.lifecycle == Lifecycle::Live ||
             (includeRetiring &&
              record.lifecycle == Lifecycle::Retiring))) {
            return &record;
        }
    }
    return nullptr;
}

bool EffectParamTargetStore::SlotReserved(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot) const noexcept {
    for (const Record& record : m_records) {
        if (record.reserved && record.scope == scope &&
            record.objectId == objectId &&
            record.objectGeneration == objectGeneration &&
            record.slot == slot) {
            return true;
        }
    }
    return false;
}

bool EffectParamTargetStore::AllocateRevision(std::uint32_t* out) noexcept {
    if (out == nullptr || m_nextRevision == 0 ||
        m_nextRevision > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    *out = static_cast<std::uint32_t>(m_nextRevision++);
    return true;
}

std::uint32_t EffectParamTargetStore::CountLive() const {
    std::uint32_t n = 0;
    for (const Record& r : m_records) {
        if (r.lifecycle == Lifecycle::Live) {
            ++n;
        }
    }
    return n;
}

bool EffectParamTargetStore::SeedInstalledEffect(Scope scope,
                                                 std::uint64_t objectId,
                                                 std::uint32_t slot,
                                                 std::uint32_t effectType,
                                                 const float* paramValues,
                                                 std::uint32_t paramCount) {
    return SeedInstalledEffect(scope, objectId,
                               DefaultGeneration(scope, objectId), slot,
                               effectType, paramValues, paramCount);
}

bool EffectParamTargetStore::SeedInstalledEffect(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t effectType, const float* paramValues,
    std::uint32_t paramCount) {
    PreparedSeedToken token;
    if (!PrepareInstalledEffectSeed(scope, objectId, objectGeneration, slot,
                                    effectType, paramValues, paramCount,
                                    &token)) {
        return false;
    }
    CommitPreparedInstalledEffectSeed(&token);
    return true;
}

bool EffectParamTargetStore::CanSeedFreshEffects(
    std::uint32_t paramCount) const {
    if (m_failSeedForTests ||
        m_nextRevision > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    std::uint32_t activeUsed = 0;
    std::uint32_t physicalAvailable =
        m_totalCapacity - static_cast<std::uint32_t>(m_records.size());
    for (const Record& r : m_records) {
        if (r.lifecycle == Lifecycle::Live ||
            (r.lifecycle == Lifecycle::Free && r.reserved)) {
            ++activeUsed;
        }
        if (r.lifecycle == Lifecycle::Free && !r.reserved) {
            ++physicalAvailable;
        }
    }
    return activeUsed <= m_activeCapacity &&
           paramCount <= m_activeCapacity - activeUsed &&
           paramCount <= physicalAvailable;
}

bool EffectParamTargetStore::CanSeedInstalledEffect(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t effectType, std::uint32_t paramCount) const {
    return CanSeedInstalledEffect(scope, objectId,
                                  DefaultGeneration(scope, objectId), slot,
                                  effectType, paramCount);
}

bool EffectParamTargetStore::CanSeedInstalledEffect(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t effectType,
    std::uint32_t paramCount) const {
    if (m_failSeedForTests || paramCount > kPreparedSeedMaxParams ||
        m_nextRevision > (std::numeric_limits<std::uint32_t>::max)() ||
        SlotReserved(scope, objectId, objectGeneration, slot) ||
        ForeignClaimInSlot(scope, objectId, objectGeneration, slot)) {
        return false;
    }
    for (std::uint32_t i = 0; i < paramCount; ++i) {
        if (FindEffectParamMeta(effectType, i) == nullptr) {
            return false;
        }
    }
    std::uint32_t activeUsed = 0;
    std::uint32_t replacedLive = 0;
    std::uint32_t physicalAvailable =
        m_totalCapacity - static_cast<std::uint32_t>(m_records.size());
    for (const Record& r : m_records) {
        if (r.lifecycle == Lifecycle::Live ||
            (r.lifecycle == Lifecycle::Free && r.reserved)) {
            ++activeUsed;
        }
        if (r.lifecycle == Lifecycle::Retiring && r.scope == scope &&
            r.objectId == objectId &&
            r.objectGeneration == objectGeneration && r.slot == slot) {
            return false;
        }
        if (r.reserved) {
            continue;
        }
        if (r.lifecycle == Lifecycle::Live) {
            if (r.scope == scope && r.objectId == objectId &&
                r.objectGeneration == objectGeneration && r.slot == slot) {
                ++replacedLive;
                ++physicalAvailable;
            }
        } else if (r.lifecycle == Lifecycle::Free) {
            ++physicalAvailable;
        }
    }
    if (replacedLive > activeUsed) {
        return false;
    }
    const std::uint32_t retainedActive = activeUsed - replacedLive;
    return retainedActive <= m_activeCapacity &&
           paramCount <= m_activeCapacity - retainedActive &&
           paramCount <= physicalAvailable;
}

bool EffectParamTargetStore::PrepareInstalledEffectSeed(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t effectType, const float* paramValues,
    std::uint32_t paramCount, PreparedSeedToken* outToken) {
    return PrepareInstalledEffectSeed(
        scope, objectId, DefaultGeneration(scope, objectId), slot,
        effectType, paramValues, paramCount, outToken);
}

bool EffectParamTargetStore::PrepareInstalledEffectSeed(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t effectType, const float* paramValues,
    std::uint32_t paramCount, PreparedSeedToken* outToken) {
    if (outToken == nullptr || outToken->m_prepared ||
        paramCount > kPreparedSeedMaxParams ||
        (paramCount != 0 && paramValues == nullptr) ||
        !CanSeedInstalledEffect(scope, objectId, objectGeneration, slot,
                                effectType, paramCount)) {
        return false;
    }
    if (m_prepareSeedFailureAfterForTests == 0) {
        return false;
    }
    if (m_prepareSeedFailureAfterForTests > 0) {
        --m_prepareSeedFailureAfterForTests;
    }

    PreparedSeedToken prepared;
    prepared.m_scope = scope;
    prepared.m_objectId = objectId;
    prepared.m_objectGeneration = objectGeneration;
    prepared.m_slot = slot;
    prepared.m_effectType = effectType;
    prepared.m_paramCount = paramCount;
    if (!AllocateRevision(&prepared.m_revision)) {
        return false;
    }

    for (std::uint32_t i = 0; i < m_records.size(); ++i) {
        Record& record = m_records[i];
        if (record.lifecycle == Lifecycle::Live && !record.reserved &&
            record.scope == scope && record.objectId == objectId &&
            record.objectGeneration == objectGeneration &&
            record.slot == slot) {
            if (prepared.m_slotRecordCount >=
                prepared.m_slotRecordIndices.size()) {
                return false;
            }
            record.reserved = true;
            prepared.m_slotRecordIndices[prepared.m_slotRecordCount++] = i;
        }
    }
    std::uint32_t selected = 0;
    for (std::uint32_t i = 0;
         i < prepared.m_slotRecordCount && selected < paramCount; ++i) {
        prepared.m_recordIndices[selected++] =
            prepared.m_slotRecordIndices[i];
    }
    for (std::uint32_t i = 0;
         i < m_records.size() && selected < paramCount; ++i) {
        Record& record = m_records[i];
        if (record.lifecycle == Lifecycle::Free && !record.reserved) {
            record.reserved = true;
            record.scope = scope;
            record.objectId = objectId;
            record.objectGeneration = objectGeneration;
            record.slot = slot;
            prepared.m_recordIndices[selected++] = i;
        }
    }
    while (selected < paramCount && m_records.size() < m_totalCapacity) {
        m_records.emplace_back();
        Record& record = m_records.back();
        record.reserved = true;
        record.scope = scope;
        record.objectId = objectId;
        record.objectGeneration = objectGeneration;
        record.slot = slot;
        prepared.m_recordIndices[selected++] =
            static_cast<std::uint32_t>(m_records.size() - 1);
    }
    if (selected != paramCount) {
        prepared.m_paramCount = selected;
        prepared.m_prepared = true;
        CancelPreparedInstalledEffectSeed(&prepared);
        return false;
    }
    for (std::uint32_t i = 0; i < paramCount; ++i) {
        prepared.m_paramValues[i] = paramValues[i];
    }
    prepared.m_prepared = true;
    *outToken = prepared;
    return true;
}

void EffectParamTargetStore::CancelPreparedInstalledEffectSeed(
    PreparedSeedToken* token) noexcept {
    if (token == nullptr || !token->m_prepared) {
        return;
    }
    for (std::uint32_t i = 0; i < token->m_paramCount; ++i) {
        const std::uint32_t index = token->m_recordIndices[i];
        assert(index < m_records.size());
        if (index < m_records.size()) {
            m_records[index].reserved = false;
        }
    }
    for (std::uint32_t i = 0; i < token->m_slotRecordCount; ++i) {
        const std::uint32_t index = token->m_slotRecordIndices[i];
        assert(index < m_records.size());
        if (index < m_records.size()) {
            m_records[index].reserved = false;
        }
    }
    *token = PreparedSeedToken{};
}

void EffectParamTargetStore::CommitPreparedInstalledEffectSeed(
    PreparedSeedToken* token) noexcept {
    assert(token != nullptr && token->m_prepared);
    if (token == nullptr || !token->m_prepared) {
        std::terminate();
    }
    for (Record& record : m_records) {
        if (record.lifecycle == Lifecycle::Live &&
            record.scope == token->m_scope &&
            record.objectId == token->m_objectId &&
            record.objectGeneration == token->m_objectGeneration &&
            record.slot == token->m_slot) {
            CleanDirty(record);
            record.lifecycle = Lifecycle::Free;
        }
    }
    for (std::uint32_t i = 0; i < token->m_paramCount; ++i) {
        const std::uint32_t index = token->m_recordIndices[i];
        assert(index < m_records.size() && m_records[index].reserved);
        const EffectParamMeta* meta =
            FindEffectParamMeta(token->m_effectType, i);
        assert(meta != nullptr);
        if (index >= m_records.size() || !m_records[index].reserved ||
            meta == nullptr) {
            std::terminate();
        }
        Record& record = m_records[index];
        record = Record{};
        record.lifecycle = Lifecycle::Live;
        record.scope = token->m_scope;
        record.objectId = token->m_objectId;
        record.objectGeneration = token->m_objectGeneration;
        record.slot = token->m_slot;
        record.paramIndex = i;
        record.effectType = token->m_effectType;
        record.combine = meta->combine;
        record.neutral = meta->neutral;
        record.clampMin = meta->clampMin;
        record.clampMax = meta->clampMax;
        record.base = token->m_paramValues[i];
        record.contribution = NeutralOf(meta->combine);
        record.rebuildAccum = record.contribution;
        record.meterProbeContribution = NeutralOf(meta->combine);
        record.meterProbeRebuild = record.meterProbeContribution;
        record.lastCommitted = ReduceRecord(record);
        record.revision = token->m_revision;
    }
    for (std::uint32_t i = 0; i < token->m_slotRecordCount; ++i) {
        const std::uint32_t index = token->m_slotRecordIndices[i];
        if (index < m_records.size()) {
            m_records[index].reserved = false;
        }
    }
    *token = PreparedSeedToken{};
}

bool EffectParamTargetStore::ReplacePreparedInstalledEffectSeed(
    PreparedSeedToken* token, std::uint32_t effectType,
    const float* paramValues, std::uint32_t paramCount) {
    if (token == nullptr || !token->m_prepared ||
        paramCount > kPreparedSeedMaxParams ||
        (paramCount != 0 && paramValues == nullptr)) {
        return false;
    }
    for (std::uint32_t i = 0; i < paramCount; ++i) {
        if (FindEffectParamMeta(effectType, i) == nullptr) {
            return false;
        }
    }
    if (m_prepareSeedFailureAfterForTests == 0 ||
        m_nextRevision > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    std::uint32_t activeCount = 0;
    std::uint32_t replacedCount = 0;
    std::uint32_t available = token->m_paramCount;
    for (std::uint32_t old = 0; old < token->m_slotRecordCount; ++old) {
        bool selected = false;
        for (std::uint32_t current = 0; current < token->m_paramCount;
             ++current) {
            selected = selected ||
                       token->m_slotRecordIndices[old] ==
                           token->m_recordIndices[current];
        }
        available += selected ? 0u : 1u;
    }
    for (const Record& record : m_records) {
        if (record.lifecycle == Lifecycle::Live) {
            ++activeCount;
            if (record.scope == token->m_scope &&
                record.objectId == token->m_objectId &&
                record.objectGeneration == token->m_objectGeneration &&
                record.slot == token->m_slot) {
                ++replacedCount;
            }
        }
        if (!record.reserved && record.lifecycle == Lifecycle::Free) {
            ++available;
        }
    }
    available += static_cast<std::uint32_t>(
        m_totalCapacity - m_records.size());
    if (replacedCount > activeCount ||
        activeCount - replacedCount > m_activeCapacity ||
        paramCount > m_activeCapacity - (activeCount - replacedCount) ||
        paramCount > available) {
        return false;
    }
    if (m_prepareSeedFailureAfterForTests > 0) {
        --m_prepareSeedFailureAfterForTests;
    }
    std::uint32_t selected = std::min(token->m_paramCount, paramCount);
    const auto alreadySelected = [&](std::uint32_t index) {
        for (std::uint32_t i = 0; i < selected; ++i) {
            if (token->m_recordIndices[i] == index) {
                return true;
            }
        }
        return false;
    };
    for (std::uint32_t i = 0;
         i < token->m_slotRecordCount && selected < paramCount; ++i) {
        const std::uint32_t index = token->m_slotRecordIndices[i];
        if (!alreadySelected(index)) {
            token->m_recordIndices[selected++] = index;
        }
    }
    for (std::uint32_t i = 0;
         i < m_records.size() && selected < paramCount; ++i) {
        Record& record = m_records[i];
        if (!record.reserved && record.lifecycle == Lifecycle::Free) {
            record.reserved = true;
            record.scope = token->m_scope;
            record.objectId = token->m_objectId;
            record.objectGeneration = token->m_objectGeneration;
            record.slot = token->m_slot;
            token->m_recordIndices[selected++] = i;
        }
    }
    while (selected < paramCount && m_records.size() < m_totalCapacity) {
        m_records.emplace_back();
        Record& record = m_records.back();
        record.reserved = true;
        record.scope = token->m_scope;
        record.objectId = token->m_objectId;
        record.objectGeneration = token->m_objectGeneration;
        record.slot = token->m_slot;
        token->m_recordIndices[selected++] =
            static_cast<std::uint32_t>(m_records.size() - 1);
    }
    assert(selected == paramCount);
    for (std::uint32_t i = paramCount; i < token->m_paramCount; ++i) {
        const std::uint32_t index = token->m_recordIndices[i];
        assert(index < m_records.size() && m_records[index].reserved);
        bool belongsToOldSlot = false;
        for (std::uint32_t old = 0; old < token->m_slotRecordCount; ++old) {
            belongsToOldSlot = belongsToOldSlot ||
                               token->m_slotRecordIndices[old] == index;
        }
        if (!belongsToOldSlot) {
            m_records[index].reserved = false;
        }
    }
    std::uint32_t revision = 0;
    if (!AllocateRevision(&revision)) {
        return false;
    }
    token->m_effectType = effectType;
    token->m_paramCount = paramCount;
    token->m_revision = revision;
    for (std::uint32_t i = 0; i < paramCount; ++i) {
        token->m_paramValues[i] = paramValues[i];
    }
    return true;
}

bool EffectParamTargetStore::PrimeContribution(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t paramIndex, std::uint32_t effectType, float contribution,
    float* outResolved) {
    Record* r = Find(scope, objectId, slot, paramIndex);
    if (r == nullptr || r->effectType != effectType ||
        r->combine == EffectParamCombine::None || outResolved == nullptr) {
        return false;
    }
    r->contribution = contribution;
    r->rebuildAccum = contribution;
    r->bindingActive = true;
    r->foldedThisSweep = true;
    const float resolved = ReduceRecord(*r);
    r->lastCommitted = resolved;
    CleanDirty(*r);
    *outResolved = resolved;
    return true;
}

void EffectParamTargetStore::ClearSlot(Scope scope, std::uint64_t objectId,
                                       std::uint32_t slot) {
    ClearSlot(scope, objectId, DefaultGeneration(scope, objectId), slot);
}

void EffectParamTargetStore::ClearSlot(Scope scope, std::uint64_t objectId,
                                       std::uint32_t objectGeneration,
                                       std::uint32_t slot) {
    for (Record& r : m_records) {
        if (r.lifecycle == Lifecycle::Live && r.scope == scope &&
            r.objectId == objectId &&
            r.objectGeneration == objectGeneration && r.slot == slot) {
            CleanDirty(r);
            if (r.preparedOwner.Valid()) {
                r.lifecycle = Lifecycle::Retiring;
                r.reserved = false;
            } else {
                r = Record{};
            }
        }
    }
}

void EffectParamTargetStore::ClearObject(Scope scope, std::uint64_t objectId) {
    ClearObject(scope, objectId, DefaultGeneration(scope, objectId));
}

void EffectParamTargetStore::ClearObject(Scope scope,
                                         std::uint64_t objectId,
                                         std::uint32_t objectGeneration) {
    for (Record& r : m_records) {
        if (r.lifecycle == Lifecycle::Live && r.scope == scope &&
            r.objectId == objectId &&
            r.objectGeneration == objectGeneration) {
            CleanDirty(r);
            if (r.preparedOwner.Valid()) {
                r.lifecycle = Lifecycle::Retiring;
                r.reserved = false;
            } else {
                r = Record{};
            }
        }
    }
}

void EffectParamTargetStore::NoteDirect(Scope scope, std::uint64_t objectId,
                                        std::uint32_t slot,
                                        std::uint32_t paramIndex,
                                        float baseValue,
                                        float committedResolved) {
    Record* r = Find(scope, objectId, slot, paramIndex);
    if (r == nullptr) {
        return;
    }
    // The direct setter published committedResolved in the same call: retarget
    // the base, adopt the published value as the changed-only reference, and
    // clean the dirty state that publish consumed.
    r->base = baseValue;
    r->lastCommitted = committedResolved;
    CleanDirty(*r);
}

void EffectParamTargetStore::NoteDirectBound(Scope scope,
                                             std::uint64_t objectId,
                                             std::uint32_t slot,
                                             std::uint32_t paramIndex,
                                             float baseValue) {
    Record* r = Find(scope, objectId, slot, paramIndex);
    if (r == nullptr) {
        return;
    }
    // Bound: retarget the base and mark dirty; the composed value publishes
    // through the next fold + FlushDirty, so the direct call itself emits
    // nothing (no double-emit).
    r->base = baseValue;
    SetDirty(*r);
}

void EffectParamTargetStore::MarkDropped(Scope scope, std::uint64_t objectId,
                                         std::uint32_t slot,
                                         std::uint32_t paramIndex) {
    Record* r = Find(scope, objectId, slot, paramIndex);
    if (r != nullptr) {
        CleanDirty(*r);
    }
}

bool EffectParamTargetStore::CanBind(Scope scope, std::uint64_t objectId,
                                     const EffectParamLocator& loc) const {
    const Record* r = Find(scope, objectId, loc.slot, loc.paramIndex);
    if (r == nullptr) {
        return false; // empty slot or out-of-range parameter index
    }
    if (r->effectType != loc.effectType) {
        return false; // installed effect is a different type than expected
    }
    return r->combine != EffectParamCombine::None; // None: binding unsupported
}

void EffectParamTargetStore::BeginContributionSweep() {
    for (Record& r : m_records) {
        if (r.lifecycle == Lifecycle::Live) {
            r.foldedThisSweep = false;
        }
    }
}

void EffectParamTargetStore::AccumulateBinding(Scope scope,
                                               std::uint64_t objectId,
                                               std::uint32_t slot,
                                               std::uint32_t paramIndex,
                                               std::uint32_t effectType,
                                               float curveOutput) {
    Record* r = Find(scope, objectId, slot, paramIndex);
    if (r == nullptr || r->effectType != effectType) {
        return; // no record, or the installed effect is a different type
    }
    if (!r->foldedThisSweep) {
        // First contribution this sweep: reset to the domain neutral, so
        // additional bindings on the same parameter accumulate onto it.
        r->rebuildAccum = NeutralOf(r->combine);
        r->foldedThisSweep = true;
    }
    if (r->combine == EffectParamCombine::Multiply) {
        r->rebuildAccum *= curveOutput;
    } else {
        // AddDb / AddLogHz / AddLogQ / AddLogTime all sum in their own units;
        // None never reaches here (a binding on it is refused at attach).
        r->rebuildAccum += curveOutput;
    }
}

void EffectParamTargetStore::BeginMeterProbeSweep() {
    for (Record& r : m_records) {
        if (r.lifecycle == Lifecycle::Live) {
            r.meterProbeFoldedThisSweep = false;
        }
    }
}

void EffectParamTargetStore::AccumulateMeterProbe(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t paramIndex, std::uint32_t effectType,
    VnObjectId, float value) {
    Record* r = Find(scope, objectId, slot, paramIndex);
    if (r == nullptr || r->effectType != effectType) {
        return;
    }
    if (!r->meterProbeFoldedThisSweep) {
        r->meterProbeRebuild = NeutralOf(r->combine);
        r->meterProbeFoldedThisSweep = true;
    }
    if (r->combine == EffectParamCombine::Multiply) {
        r->meterProbeRebuild *= value;
    } else {
        r->meterProbeRebuild += value;
    }
}

void EffectParamTargetStore::EndMeterProbeSweep() {
    for (Record& r : m_records) {
        if (r.lifecycle != Lifecycle::Live) {
            continue;
        }
        const float folded = r.meterProbeFoldedThisSweep
                                 ? r.meterProbeRebuild
                                 : NeutralOf(r.combine);
        r.meterProbeActive = r.meterProbeFoldedThisSweep;
        if (r.meterProbeContribution != folded) {
            r.meterProbeContribution = folded;
            SetDirty(r);
        }
    }
}

void EffectParamTargetStore::ClearMeterProbeContribution(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t paramIndex, std::uint32_t effectType,
    VnObjectId) {
    Record* r = Find(scope, objectId, slot, paramIndex);
    if (r == nullptr || r->effectType != effectType) {
        return;
    }
    const float neutral = NeutralOf(r->combine);
    if (r->meterProbeActive || r->meterProbeContribution != neutral) {
        r->meterProbeActive = false;
        r->meterProbeContribution = neutral;
        SetDirty(*r);
    }
}

void EffectParamTargetStore::EndContributionSweep() {
    for (Record& r : m_records) {
        if (r.lifecycle != Lifecycle::Live) {
            continue;
        }
        // A record folded this sweep takes its accumulation; one whose bindings
        // all detached (or whose variables have no value) returns to the domain
        // neutral, so its resolved value returns to the base alone.
        const float folded =
            r.foldedThisSweep ? r.rebuildAccum : NeutralOf(r.combine);
        r.bindingActive = r.foldedThisSweep;
        if (r.contribution != folded) {
            r.contribution = folded;
            SetDirty(r);
        }
    }
}

float EffectParamTargetStore::ReduceRecord(const Record& r) const {
    float value = r.base;
    switch (r.combine) {
    case EffectParamCombine::AddDb:
        value = r.base + r.contribution + r.meterProbeContribution;
        break;
    case EffectParamCombine::Multiply:
        value = r.base * r.contribution * r.meterProbeContribution;
        break;
    case EffectParamCombine::AddLogHz:
    case EffectParamCombine::AddLogQ:
    case EffectParamCombine::AddLogTime:
        value = r.base *
                std::exp2(r.contribution + r.meterProbeContribution);
        break;
    case EffectParamCombine::None:
    default:
        value = r.base;
        break;
    }
    float clampMin = r.clampMin;
    if (r.clampMin == kEffectParamClampStrictlyPositive) {
        // Exact validator parity: "value > 0" includes subnormals, so the
        // smallest positive subnormal is the only nonzero lower bound that does
        // not reject or alter a finite value accepted by the released validator.
        clampMin = std::numeric_limits<float>::denorm_min();
    }
    float clampMax = r.clampMax;
    if (r.clampMax == kEffectParamClampNyquist) {
        // Strictly below Nyquist: the released validator requires a frequency
        // < nyquist, and a composed value that reaches the audio thread on
        // opcode 14/22 bypasses that validator, so the clamp itself must land
        // below Nyquist (not at it).
        clampMax = std::nextafter(0.5f * static_cast<float>(m_sampleRate), 0.0f);
    } else if (r.clampMax == kEffectParamClampStrictlyBelowOne) {
        // Exact open-one parity: use the largest representable float below
        // unity so every finite value accepted by "value < 1" remains valid.
        clampMax = std::nextafter(1.0f, 0.0f);
    }
    value = std::max(clampMin, std::min(clampMax, value));
    return value;
}

float EffectParamTargetStore::Reduce(Scope scope, std::uint64_t objectId,
                                     std::uint32_t slot,
                                     std::uint32_t paramIndex) const {
    const Record* r = Find(scope, objectId, slot, paramIndex);
    return r == nullptr ? 0.0f : ReduceRecord(*r);
}

bool EffectParamTargetStore::PendingPublication(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t paramIndex) const {
    const Record* r = Find(scope, objectId, slot, paramIndex);
    return r != nullptr && r->dirty;
}

float EffectParamTargetStore::LastCommitted(Scope scope, std::uint64_t objectId,
                                            std::uint32_t slot,
                                            std::uint32_t paramIndex) const {
    const Record* r = Find(scope, objectId, slot, paramIndex);
    return r == nullptr ? 0.0f : r->lastCommitted;
}

bool EffectParamTargetStore::TryClaim(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t paramIndex, TargetClaimOwner owner,
    std::uint32_t expectedRevision) noexcept {
    return TryClaim(scope, objectId, DefaultGeneration(scope, objectId), slot,
                    paramIndex, owner, expectedRevision);
}

bool EffectParamTargetStore::TryClaim(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t paramIndex, TargetClaimOwner owner,
    std::uint32_t expectedRevision) noexcept {
    if (!CanClaim(scope, objectId, objectGeneration, slot, paramIndex, owner, expectedRevision)) return false;
    Record* record = FindExact(scope, objectId, objectGeneration, slot,
                               paramIndex, false);
    if (record->preparedOwner.Valid()) {
        ++record->publicationRefCount;
        return true;
    }
    record->preparedOwner = owner;
    record->publicationRefCount = owner.Shared() ? 1u : 0u;
    return true;
}

bool EffectParamTargetStore::CanClaim(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t paramIndex, TargetClaimOwner owner,
    std::uint32_t expectedRevision) const noexcept {
    const auto* record = FindExact(scope, objectId, objectGeneration, slot, paramIndex, false);
    if (record == nullptr || !owner.Valid() || record->revision != expectedRevision ||
        SlotReserved(scope, objectId, objectGeneration, slot)) return false;
    return record->preparedOwner.Valid()
        ? owner.Shared() && record->preparedOwner == owner && record->publicationRefCount != UINT32_MAX
        : record->publicationRefCount == 0;
}

void EffectParamTargetStore::ReleaseClaim(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t paramIndex, TargetClaimOwner owner) noexcept {
    ReleaseClaim(scope, objectId, DefaultGeneration(scope, objectId), slot,
                 paramIndex, owner);
}

void EffectParamTargetStore::ReleaseClaim(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t paramIndex,
    TargetClaimOwner owner) noexcept {
    Record* record = FindExact(scope, objectId, objectGeneration, slot,
                               paramIndex, true);
    if (record != nullptr && record->preparedOwner == owner) {
        if (owner.Shared() && (record->publicationRefCount == 0 ||
            --record->publicationRefCount != 0)) return;
        if (record->lifecycle == Lifecycle::Retiring) {
            *record = Record{};
        } else {
            record->preparedOwner = {};
        }
    }
}

bool EffectParamTargetStore::SnapshotForClaim(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t paramIndex, TargetClaimOwner owner, float* resolved,
    std::uint32_t* revision, std::uint32_t* effectType) const noexcept {
    return SnapshotForClaim(
        scope, objectId, DefaultGeneration(scope, objectId), slot,
        paramIndex, owner, resolved, revision, effectType);
}

bool EffectParamTargetStore::SnapshotForClaim(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t paramIndex, TargetClaimOwner owner,
    float* resolved, std::uint32_t* revision,
    std::uint32_t* effectType) const noexcept {
    const Record* record = FindExact(scope, objectId, objectGeneration, slot,
                                     paramIndex, true);
    if (record == nullptr || resolved == nullptr || revision == nullptr ||
        effectType == nullptr || record->preparedOwner != owner) {
        return false;
    }
    *resolved = ReduceRecord(*record);
    *revision = record->revision;
    *effectType = record->effectType;
    return true;
}

std::uint32_t EffectParamTargetStore::TargetRevision(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t paramIndex) const noexcept {
    return TargetRevision(scope, objectId, DefaultGeneration(scope, objectId),
                          slot, paramIndex);
}

std::uint32_t EffectParamTargetStore::TargetRevision(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t paramIndex) const noexcept {
    const Record* record = FindExact(scope, objectId, objectGeneration, slot,
                                     paramIndex, true);
    return record == nullptr ? 0 : record->revision;
}

bool EffectParamTargetStore::ExactTargetRevision(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t paramIndex, std::uint32_t expectedEffectType,
    std::uint32_t* revision) const noexcept {
    return ExactTargetRevision(
        scope, objectId, DefaultGeneration(scope, objectId), slot,
        paramIndex, expectedEffectType, revision);
}

bool EffectParamTargetStore::ExactTargetRevision(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t paramIndex,
    std::uint32_t expectedEffectType,
    std::uint32_t* revision) const noexcept {
    if (revision == nullptr) {
        return false;
    }
    *revision = 0;
    const Record* record = FindExact(scope, objectId, objectGeneration, slot,
                                     paramIndex, false);
    if (record == nullptr || record->effectType != expectedEffectType ||
        record->combine == EffectParamCombine::None) {
        return false;
    }
    *revision = record->revision;
    return true;
}

bool EffectParamTargetStore::ForeignClaim(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t paramIndex) const noexcept {
    return ForeignClaim(scope, objectId, DefaultGeneration(scope, objectId),
                        slot, paramIndex);
}

bool EffectParamTargetStore::ForeignClaim(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t paramIndex) const noexcept {
    if (SlotReserved(scope, objectId, objectGeneration, slot)) {
        return true;
    }
    const Record* record = FindExact(scope, objectId, objectGeneration, slot,
                                     paramIndex, true);
    return record != nullptr && record->preparedOwner.Valid();
}

bool EffectParamTargetStore::ForeignClaimInSlot(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot) const noexcept {
    if (SlotReserved(scope, objectId, objectGeneration, slot)) {
        return true;
    }
    return ClaimedInSlot(scope, objectId, objectGeneration, slot);
}

bool EffectParamTargetStore::ClaimedInSlot(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot) const noexcept {
    for (const Record& record : m_records) {
        if ((record.lifecycle == Lifecycle::Live ||
             record.lifecycle == Lifecycle::Retiring) &&
            record.scope == scope && record.objectId == objectId &&
            record.objectGeneration == objectGeneration &&
            record.slot == slot && record.preparedOwner.Valid()) {
            return true;
        }
    }
    return false;
}

void EffectParamTargetStore::MarkSliceCommitted(
    Scope scope, std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t paramIndex, TargetClaimOwner owner, float resolved) noexcept {
    MarkSliceCommitted(scope, objectId, DefaultGeneration(scope, objectId),
                       slot, paramIndex, owner, resolved);
}

void EffectParamTargetStore::MarkSliceCommitted(
    Scope scope, std::uint64_t objectId, std::uint32_t objectGeneration,
    std::uint32_t slot, std::uint32_t paramIndex, TargetClaimOwner owner,
    float resolved) noexcept {
    Record* record = FindExact(scope, objectId, objectGeneration, slot,
                               paramIndex, true);
    if (record == nullptr || record->preparedOwner != owner) {
        return;
    }
    if (owner.Shared()) {
        if (record->lifecycle != Lifecycle::Retiring) {
            record->lastCommitted = resolved;
            if (ReduceRecord(*record) != resolved) SetDirty(*record);
            else CleanDirty(*record);
        }
        ReleaseClaim(scope, objectId, objectGeneration, slot, paramIndex, owner);
        return;
    }
    if (record->lifecycle == Lifecycle::Retiring) {
        *record = Record{};
    } else {
        record->lastCommitted = resolved;
        if (ReduceRecord(*record) != resolved) {
            SetDirty(*record);
        } else {
            CleanDirty(*record);
        }
        record->preparedOwner = {};
    }
}

std::uint32_t
EffectParamTargetStore::RetiringRecordCountForTests() const noexcept {
    std::uint32_t count = 0;
    for (const Record& record : m_records) {
        count += record.lifecycle == Lifecycle::Retiring ? 1u : 0u;
    }
    return count;
}

} // namespace harken
