#include "harken/core/ProgramGenerationRegistry.h"

namespace harken {

ProgramGenerationRegistry::ProgramGenerationRegistry(std::uint32_t capacity)
    : m_entries(capacity) {}

const ProgramGenerationRegistry::Entry* ProgramGenerationRegistry::Find(
    ProgramGeneration generation) const {
    if (generation == 0) {
        return nullptr;
    }
    for (const Entry& entry : m_entries) {
        if (entry.used && entry.generation == generation) {
            return &entry;
        }
    }
    return nullptr;
}

ProgramGeneration ProgramGenerationRegistry::Publish(
    std::uint64_t payload, std::uint64_t audioSequenceNow) {
    // Find room before touching anything: a refused publish must leave the
    // caller with the state it already had, not a partially installed set.
    Entry* free = nullptr;
    for (Entry& entry : m_entries) {
        if (!entry.used) {
            free = &entry;
            break;
        }
    }
    if (free == nullptr || m_generationExhausted) {
        return 0;
    }
    const IssueResult issue =
        TryIssue(&m_nextGeneration, &m_generationExhausted);
    if (issue.status != IssueStatus::Ok) {
        return 0;
    }
    // The outgoing generation stays readable and takes the fence it must be
    // seen past.
    for (Entry& entry : m_entries) {
        if (entry.used && !entry.superseded && entry.generation == m_current) {
            entry.superseded = true;
            entry.supersededAtAudioSequence = audioSequenceNow;
            break;
        }
    }
    free->used = true;
    free->superseded = false;
    free->generation = issue.value;
    free->payload = payload;
    free->supersededAtAudioSequence = 0;
    m_current = free->generation;
    ++m_retainedCount;
    if (m_retainedCount > m_highWater) {
        m_highWater = m_retainedCount;
    }
    return m_current;
}

bool ProgramGenerationRegistry::IsReadable(
    ProgramGeneration generation) const {
    return Find(generation) != nullptr;
}

bool ProgramGenerationRegistry::PayloadOf(ProgramGeneration generation,
                                          std::uint64_t* outPayload) const {
    const Entry* entry = Find(generation);
    if (entry == nullptr || outPayload == nullptr) {
        return false;
    }
    *outPayload = entry->payload;
    return true;
}

std::uint32_t ProgramGenerationRegistry::ReclaimSuperseded(
    std::uint64_t observedAudioSequence) {
    std::uint32_t released = 0;
    for (Entry& entry : m_entries) {
        if (!entry.used || !entry.superseded) {
            continue;
        }
        // Strictly greater, not greater-or-equal: a reader sitting exactly at
        // the fence sequence may still be inside the block that read this
        // generation. Over-retaining by one round is the safe direction.
        if (observedAudioSequence <= entry.supersededAtAudioSequence) {
            continue;
        }
        entry = Entry{};
        --m_retainedCount;
        ++released;
    }
    return released;
}

void ProgramGenerationRegistry::BeginTransportReset(
    std::uint64_t audioSequenceNow) noexcept {
    for (Entry& entry : m_entries) {
        if (entry.used && !entry.superseded) {
            entry.superseded = true;
            entry.supersededAtAudioSequence = audioSequenceNow;
        }
    }
    m_current = 0;
}

void ProgramGenerationRegistry::ResetForTransport() noexcept {
    for (Entry& entry : m_entries) {
        entry = Entry{};
    }
    m_current = 0;
    m_retainedCount = 0;
}

} // namespace harken
