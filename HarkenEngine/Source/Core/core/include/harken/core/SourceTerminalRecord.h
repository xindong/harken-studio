#ifndef HARKEN_CORE_SOURCE_TERMINAL_RECORD_H
#define HARKEN_CORE_SOURCE_TERMINAL_RECORD_H

#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/RuntimeWorkIndex.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace harken {

struct VoiceReleaseObserver {
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t targetSequence = 0;
    bool Valid() const noexcept { return slot != UINT32_MAX && targetSequence != 0; }
};

enum class SourceTerminalKind : std::uint8_t {
    None, Natural, Stop, Cancelled, ProducerFailure, IntegrityAbort, Parked
};

struct SourceTerminalToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
    bool Valid() const noexcept { return slot != UINT32_MAX && generation != 0; }
};

struct SourceTerminalPayload {
    SourceTerminalKind kind = SourceTerminalKind::None;
    std::uint64_t frame = 0;
};

struct SourceTerminalResourceOwner {
    std::uint32_t serviceSlot = UINT32_MAX;
    std::uint64_t serviceGeneration = 0;
    std::uint64_t instance = 0;
    std::uint64_t epoch = 0;
    std::uint32_t invocationSlot = UINT32_MAX;
    std::uint64_t invocationGeneration = 0;
};

// Stable storage outlives render slots. Only its bound render owner writes the
// payload; the completed sequence publishes that immutable write to logic.
struct SourceTerminalRecord {
    std::atomic<std::uint64_t> completedSequence{0};
    std::atomic<SourceTerminalKind> bypassResult{SourceTerminalKind::None};
    SourceTerminalPayload payload{};
    std::uint64_t leaf = 0;
    std::uint64_t acceptedEpoch = 0;
    std::uint64_t render = 0;
    VoiceReleaseObserver release{};
    std::uint32_t generation = 1;
    bool occupied = false;
    bool quarantined = false;

    // Logic-owned retirement state is independent of the audio publication.
    std::uint64_t controller = 0;
    SourceTerminalResourceOwner resourceOwner{};
    std::uint64_t releaseSeenAt = 0;
    SourceTerminalKind ownerResult = SourceTerminalKind::None;
    bool releaseSeen = false;
    bool retiring = false;
    bool retryAfterMissedEntry = false;
};

class SourceTerminalRecords {
    struct ActiveFamily {};
    using ActiveIdentity = RuntimeIndexIdentity<ActiveFamily>;
public:
    explicit SourceTerminalRecords(std::uint32_t capacity) : records_(capacity) {
        // Prepare navigation alongside the fixed records, never during a
        // Source reservation or an audio completion publication.
        if (capacity == 0) return;
        if (!active_.Reserve(capacity)) throw std::bad_alloc{};
        for (std::uint64_t slot = 0; slot < capacity; slot += 256u)
            if (!active_.Prepare({1, static_cast<std::uint32_t>(slot), 0})) throw std::bad_alloc{};
    }
    std::uint32_t Capacity() const noexcept {
        return static_cast<std::uint32_t>(records_.size());
    }
    std::uint32_t OccupiedCount() const noexcept { return occupiedCount_; }
    SourceTerminalRecord* At(std::uint32_t slot) noexcept {
        return slot < records_.size() ? &records_[slot] : nullptr;
    }
    SourceTerminalRecord* Resolve(SourceTerminalToken token) noexcept {
        auto* record = At(token.slot);
        return token.Valid() && record != nullptr && record->occupied &&
            record->generation == token.generation ? record : nullptr;
    }
    const SourceTerminalRecord* Resolve(SourceTerminalToken token) const noexcept {
        return const_cast<SourceTerminalRecords*>(this)->Resolve(token);
    }
    SourceTerminalToken NextOccupied(std::uint32_t* cursor) const noexcept {
        if (cursor != nullptr && *cursor < Capacity()) {
            const auto slot = *cursor;
            const auto& record = records_[slot];
            if (record.occupied && record.generation != 0) {
                ++*cursor;
                return {slot, record.generation};
            }
        }
        const auto id = active_.Next(RuntimeActivitySet::Active, cursor,
            [this](std::uint32_t slot) noexcept {
                const auto& record = records_[slot];
                return record.occupied ? ActiveIdentity{record.generation, slot, 0} : ActiveIdentity{};
            });
        return id.Valid() ? SourceTerminalToken{id.slot, static_cast<std::uint32_t>(id.generation)}
                          : SourceTerminalToken{};
    }
    SourceTerminalToken Reserve(const PlayableInstanceTable& instances,
        std::uint64_t leaf, std::uint64_t epoch,
        std::uint64_t render, VoiceReleaseObserver release,
        std::uint64_t controller, SourceTerminalResourceOwner resourceOwner = {}) noexcept {
        // One record per real Sound slot, including unpublished reservations.
        const auto slot = instances.SourceRetirementSlot(leaf);
        auto* record = At(slot);
        if (leaf == 0 || epoch == 0 || render == 0 || !release.Valid() ||
            instances.SourceRetirementEpoch(leaf) != epoch ||
            record == nullptr || record->occupied || record->quarantined) return {};
        record->completedSequence.store(0, std::memory_order_relaxed);
        record->bypassResult.store(SourceTerminalKind::None, std::memory_order_relaxed);
        record->payload = {};
        record->leaf = leaf;
        record->acceptedEpoch = epoch;
        record->render = render;
        record->release = release;
        record->controller = controller;
        record->resourceOwner = resourceOwner;
        record->releaseSeenAt = 0;
        record->ownerResult = SourceTerminalKind::None;
        record->releaseSeen = false;
        record->retiring = false;
        record->retryAfterMissedEntry = false;
        record->occupied = true;
        if (!SetActive({slot, record->generation}, true)) {
            record->occupied = false;
            return {};
        }
        ++occupiedCount_;
        return {slot, record->generation};
    }
    bool Release(SourceTerminalToken token) noexcept {
        auto* record = Resolve(token);
        if (record == nullptr) return false;
        if (!SetActive(token, false)) return false;
        record->occupied = false;
        --occupiedCount_;
        if (record->generation == UINT32_MAX) record->quarantined = true;
        else ++record->generation;
        return true;
    }
    bool Capture(SourceTerminalToken token, std::uint64_t completedBlocks,
        SourceTerminalPayload* out) const noexcept {
        const auto* record = Resolve(token);
        if (record == nullptr || out == nullptr) return false;
        const auto sequence = record->completedSequence.load(std::memory_order_acquire);
        if (sequence == 0 || sequence > completedBlocks) return false;
        *out = record->payload;
        return out->kind != SourceTerminalKind::None;
    }
private:
    bool SetActive(SourceTerminalToken token, bool present) noexcept {
        return active_.SetMembership({token.generation, token.slot, 0}, RuntimeActivitySet::Active,
            present, [this, token](ActiveIdentity id) noexcept {
                return id.slot == token.slot && id.generation == token.generation && Resolve(token) != nullptr;
            });
    }
    std::vector<SourceTerminalRecord> records_;
    RuntimeActivityIndex<ActiveFamily> active_;
    std::uint32_t occupiedCount_ = 0;
};

} // namespace harken

#endif
