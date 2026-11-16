#include "harken/core/DeferredIntentPool.h"

#include <algorithm>
#include <cassert>

namespace harken {

DeferredIntentPool::DeferredIntentPool(std::uint32_t capacity,
                                       std::uint32_t maxPerOwner)
    : m_entries(capacity), m_maxPerOwner(maxPerOwner) {}

DeferredIntentPool::~DeferredIntentPool() {
    for (const auto& entry : m_entries) assert(entry.preparedPairOwner == nullptr);
}

std::uint32_t DeferredIntentPool::CountForOwner(
    PlayableInstanceHandle owner) const {
    return CountForOwner(DeferredIntentOwnerDomain::Playable, owner);
}

std::uint32_t DeferredIntentPool::CountForOwner(
    DeferredIntentOwnerDomain domain, std::uint64_t ownerHandle) const {
    std::uint32_t held = 0;
    for (const Entry& entry : m_entries) {
        if ((entry.state == DeferredIntentEntryState::Held ||
             entry.state == DeferredIntentEntryState::Claimed) &&
            entry.intent.ownerDomain == domain &&
            entry.intent.ownerHandle == ownerHandle) {
            ++held;
        }
    }
    return held;
}

std::uint32_t DeferredIntentPool::OccupancyForOwner(
    DeferredIntentOwnerDomain domain, std::uint64_t ownerHandle) const {
    std::uint32_t occupied = 0;
    for (const Entry& entry : m_entries) {
        if ((entry.state == DeferredIntentEntryState::Prepared ||
             entry.state == DeferredIntentEntryState::Held ||
             entry.state == DeferredIntentEntryState::Claimed) &&
            entry.intent.ownerDomain == domain &&
            entry.intent.ownerHandle == ownerHandle) {
            ++occupied;
        }
    }
    return occupied;
}

bool DeferredIntentPool::SetGenerationForTests(
    std::uint32_t slot, std::uint64_t generation) {
    if (slot >= m_entries.size() || generation == 0 ||
        m_entries[slot].state != DeferredIntentEntryState::Free) {
        return false;
    }
    m_entries[slot].generation = generation;
    return true;
}

bool DeferredIntentPool::SetTokenGenerationForTests(
    DeferredIntentToken token, std::uint64_t generation,
    DeferredIntentToken* outToken) {
    if (outToken == nullptr || generation == 0 || !token.Valid() ||
        token.slot >= m_entries.size()) {
        return false;
    }
    Entry& entry = m_entries[token.slot];
    if (entry.state != DeferredIntentEntryState::Held ||
        entry.generation != token.generation) {
        return false;
    }
    entry.generation = generation;
    *outToken = {token.slot, generation};
    return true;
}

std::uint64_t DeferredIntentPool::EarliestHeldDueBefore(
    std::uint64_t cutoffFrame) const {
    if (m_count == 0) return UINT64_MAX;
    std::uint64_t earliest = UINT64_MAX;
    for (const Entry& entry : m_entries) {
        if (entry.state == DeferredIntentEntryState::Held &&
            entry.intent.dueFrame < cutoffFrame) {
            earliest = std::min(earliest, entry.intent.dueFrame);
        }
    }
    return earliest;
}

DeferredIntentAddResult DeferredIntentPool::AddPrepared(
    const DeferredIntent& intent) {
    if (m_maxPerOwner != 0 &&
        OccupancyForOwner(intent.ownerDomain, intent.ownerHandle) >=
            m_maxPerOwner) {
        return {DeferredIntentRefusal::OwnerCapReached, {}};
    }
    for (std::uint32_t slot = 0; slot < m_entries.size(); ++slot) {
        Entry& entry = m_entries[slot];
        if (entry.state != DeferredIntentEntryState::Free) {
            continue;
        }
        entry.intent = intent;
        entry.retryReservation = {};
        entry.state = DeferredIntentEntryState::Prepared;
        ++m_preparedCount;
        return {DeferredIntentRefusal::None, {slot, entry.generation}};
    }
    return {DeferredIntentRefusal::PoolFull, {}};
}

bool DeferredIntentPool::CommitPrepared(DeferredIntentToken token) {
    Entry* entry = Validate(token, DeferredIntentEntryState::Prepared);
    if (entry == nullptr || entry->preparedPairOwner != nullptr) {
        return false;
    }
    CommitValidatedPrepared(*entry);
    return true;
}

void DeferredIntentPool::CommitValidatedPrepared(Entry& entry) noexcept {
    entry.state = DeferredIntentEntryState::Held;
    --m_preparedCount;
    ++m_count;
    m_highWater = std::max(m_highWater, m_count);
}

bool DeferredIntentPool::AbandonPrepared(DeferredIntentToken token) {
    Entry* entry = Validate(token, DeferredIntentEntryState::Prepared);
    if (entry == nullptr || entry->preparedPairOwner != nullptr) {
        return false;
    }
    entry->state = DeferredIntentEntryState::Free;
    entry->intent = {};
    entry->retryReservation = {};
    --m_preparedCount;
    return true;
}

DeferredIntentAddResult DeferredIntentPool::AddClaimable(
    const DeferredIntent& intent) {
    DeferredIntentAddResult added = AddPrepared(intent);
    if (added.status != DeferredIntentRefusal::None) {
        return added;
    }
    if (!CommitPrepared(added.token)) {
        (void)AbandonPrepared(added.token);
        return {DeferredIntentRefusal::ProtocolViolation, {}};
    }
    return added;
}

DeferredIntentPool::Entry* DeferredIntentPool::Validate(
    DeferredIntentToken token, DeferredIntentEntryState expected) {
    if (!token.Valid() || token.slot >= m_entries.size()) {
        return nullptr;
    }
    Entry& entry = m_entries[token.slot];
    return entry.generation == token.generation && entry.state == expected
               ? &entry
               : nullptr;
}

void DeferredIntentPool::FreeEntry(Entry& entry) {
    entry.retryReservation = {};
    if (entry.generation == UINT64_MAX) {
        entry.state = DeferredIntentEntryState::Quarantined;
        entry.intent = {};
    } else {
        ++entry.generation;
        entry.state = DeferredIntentEntryState::Free;
        entry.intent = {};
    }
    --m_count;
}

std::uint32_t DeferredIntentPool::ClaimDue(
    std::uint64_t windowEndFrame, const PlayableInstanceTable& instances,
    DeferredIntentClaim* out, std::uint32_t capacity,
    std::uint32_t* staleDropped, std::uint32_t* dueCount,
    std::uint64_t currentQuantum,
    std::vector<DeferredIntentClaim>* staleClaims) {
    std::uint32_t claimed = 0;
    std::uint32_t dropped = 0;
    std::uint32_t due = 0;
    const auto scanCapacity = m_count == 0 ? std::size_t{0} : m_entries.size();
    const auto precedes = [](const DeferredIntent& a,
                             const DeferredIntent& b) {
        if (a.dueFrame != b.dueFrame) {
            return a.dueFrame < b.dueFrame;
        }
        if (a.commitSequence != b.commitSequence) {
            return a.commitSequence < b.commitSequence;
        }
        if (a.executionOrdinal != b.executionOrdinal) {
            return a.executionOrdinal < b.executionOrdinal;
        }
        return a.payload < b.payload;
    };
    for (std::uint32_t slot = 0; slot < scanCapacity; ++slot) {
        Entry& entry = m_entries[slot];
        if (entry.state != DeferredIntentEntryState::Held ||
            entry.intent.dueFrame >= windowEndFrame) {
            continue;
        }
        if (entry.intent.ownerDomain ==
                DeferredIntentOwnerDomain::Playable &&
            !instances.ValidateStamp(entry.intent.ownerHandle,
                                     entry.intent.ownerEpoch)) {
            if (staleClaims != nullptr)
                staleClaims->push_back({{slot, entry.generation}, entry.intent});
            FreeEntry(entry);
            ++dropped;
            continue;
        }
        ++due;
        if (entry.retryReservation.Valid()) {
            continue;
        }
        bool classBlocked = false;
        for (std::uint32_t blockerSlot = 0;
             blockerSlot < m_entries.size(); ++blockerSlot) {
            const Entry& blocker = m_entries[blockerSlot];
            if (blocker.state != DeferredIntentEntryState::Held ||
                blocker.intent.dueFrame != entry.intent.dueFrame ||
                (!blocker.retryReservation.Valid() &&
                 (currentQuantum == UINT64_MAX ||
                  blocker.intent.notBeforeQuantum <= currentQuantum))) {
                continue;
            }
            const bool sameOrder =
                !precedes(blocker.intent, entry.intent) &&
                !precedes(entry.intent, blocker.intent);
            if (precedes(blocker.intent, entry.intent) ||
                (sameOrder && blockerSlot <= slot)) {
                classBlocked = true;
                break;
            }
        }
        if (classBlocked) {
            continue;
        }
        if (capacity == 0 || out == nullptr) {
            continue;
        }
        const DeferredIntentClaim candidate = {
            {slot, entry.generation}, entry.intent};
        std::uint32_t insertion = claimed;
        while (insertion > 0 &&
               precedes(candidate.intent, out[insertion - 1].intent)) {
            --insertion;
        }
        if (claimed < capacity) {
            for (std::uint32_t i = claimed; i > insertion; --i) {
                out[i] = out[i - 1];
            }
            out[insertion] = candidate;
            ++claimed;
        } else if (insertion < capacity) {
            for (std::uint32_t i = capacity - 1; i > insertion; --i) {
                out[i] = out[i - 1];
            }
            out[insertion] = candidate;
        }
    }
    for (std::uint32_t i = 0; i < claimed; ++i) {
        m_entries[out[i].token.slot].state = DeferredIntentEntryState::Claimed;
    }
    if (staleDropped != nullptr) {
        *staleDropped = dropped;
    }
    if (dueCount != nullptr) {
        *dueCount = due;
    }
    return claimed;
}

DeferredIntentRefusal DeferredIntentPool::Retry(
    DeferredIntentToken token, std::uint64_t notBeforeQuantum) {
    Entry* entry = Validate(token, DeferredIntentEntryState::Claimed);
    if (entry == nullptr) {
        return DeferredIntentRefusal::ProtocolViolation;
    }
    entry->intent.notBeforeQuantum = notBeforeQuantum;
    entry->state = DeferredIntentEntryState::Held;
    return DeferredIntentRefusal::None;
}

DeferredIntentAddResult DeferredIntentPool::Replace(
    DeferredIntentToken token, const DeferredIntent& intent) {
    Entry* entry = Validate(token, DeferredIntentEntryState::Claimed);
    if (entry == nullptr) {
        return {DeferredIntentRefusal::ProtocolViolation, {}};
    }
    if (entry->generation == UINT64_MAX) {
        return {DeferredIntentRefusal::GenerationExhausted, {}};
    }
    ++entry->generation;
    entry->intent = intent;
    entry->retryReservation = {};
    entry->state = DeferredIntentEntryState::Held;
    return {DeferredIntentRefusal::None,
            {token.slot, entry->generation}};
}

bool DeferredIntentPool::Release(DeferredIntentToken token) {
    Entry* entry = Validate(token, DeferredIntentEntryState::Claimed);
    if (entry == nullptr) {
        return false;
    }
    FreeEntry(*entry);
    return true;
}

bool DeferredIntentPool::Cancel(DeferredIntentToken token) {
    Entry* entry = Validate(token, DeferredIntentEntryState::Held);
    if (entry == nullptr) {
        return false;
    }
    FreeEntry(*entry);
    return true;
}

bool DeferredIntentPool::CanResetForTransport() const noexcept {
    for (const Entry& entry : m_entries) {
        if (entry.preparedPairOwner != nullptr) return false;
        if ((entry.state == DeferredIntentEntryState::Held ||
             entry.state == DeferredIntentEntryState::Claimed) &&
            entry.generation == UINT64_MAX) {
            return false;
        }
    }
    return true;
}

bool DeferredIntentPool::ResetForTransport() noexcept {
    if (!CanResetForTransport()) {
        return false;
    }
    for (Entry& entry : m_entries) {
        if (entry.state == DeferredIntentEntryState::Prepared) {
            entry.state = DeferredIntentEntryState::Free;
            entry.intent = {};
            entry.retryReservation = {};
            continue;
        }
        if (entry.state != DeferredIntentEntryState::Held &&
            entry.state != DeferredIntentEntryState::Claimed) {
            continue;
        }
        ++entry.generation;
        entry.state = DeferredIntentEntryState::Free;
        entry.intent = {};
        entry.retryReservation = {};
    }
    m_count = 0;
    m_preparedCount = 0;
    return true;
}

std::uint32_t DeferredIntentPool::Materialize(
    std::uint64_t windowEndFrame, const PlayableInstanceTable& instances,
    std::vector<DeferredIntent>* out,
    std::vector<std::uint64_t>* droppedPayloads) {
    std::uint32_t dropped = 0;
    for (Entry& entry : m_entries) {
        if (entry.state != DeferredIntentEntryState::Held ||
            entry.intent.dueFrame >= windowEndFrame) {
            continue;
        }
        // The stamp decides, not the pool's memory of who queued it: an owner
        // that ended or cancelled invalidates its intents without having been
        // walked.
        const bool live =
            entry.intent.ownerDomain == DeferredIntentOwnerDomain::Action ||
            instances.ValidateStamp(entry.intent.ownerHandle,
                                    entry.intent.ownerEpoch);
        if (live) {
            if (out == nullptr) {
                continue;
            }
            out->push_back(entry.intent);
        } else {
            if (droppedPayloads != nullptr) {
                droppedPayloads->push_back(entry.intent.payload);
            }
            ++dropped;
        }
        FreeEntry(entry);
    }
    return dropped;
}

std::uint32_t DeferredIntentPool::DropStale(
    const PlayableInstanceTable& instances,
    std::vector<std::uint64_t>* droppedPayloads,
    std::vector<DeferredIntentClaim>* staleClaims) {
    if (m_count == 0) return 0;
    std::uint32_t dropped = 0;
    for (Entry& entry : m_entries) {
        if (entry.state != DeferredIntentEntryState::Held) {
            continue;
        }
        if (entry.intent.ownerDomain == DeferredIntentOwnerDomain::Action ||
            instances.ValidateStamp(entry.intent.ownerHandle,
                                    entry.intent.ownerEpoch)) {
            continue;
        }
        if (droppedPayloads != nullptr) {
            droppedPayloads->push_back(entry.intent.payload);
        }
        if (staleClaims != nullptr)
            staleClaims->push_back({{static_cast<std::uint32_t>(
                &entry - m_entries.data()), entry.generation}, entry.intent});
        FreeEntry(entry);
        ++dropped;
    }
    return dropped;
}

} // namespace harken
