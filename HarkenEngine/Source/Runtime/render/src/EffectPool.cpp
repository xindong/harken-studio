#include "harken/render/EffectPool.h"
#include "harken/core/MonitorLifecycleSink.h"
#include "harken/core/LogicAllocCheckpoint.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace harken {

namespace {

// Whole std::max_align_t units needed to hold one instance of `stateBytes`. Each
// instance occupies whole units, so its base address is max_align_t-aligned —
// which the registry pins to be at least every state type's alignment.
std::uint32_t StrideUnits(std::uint32_t stateBytes) {
    const std::uint32_t unit =
        static_cast<std::uint32_t>(sizeof(std::max_align_t));
    return (stateBytes + unit - 1u) / unit;
}

} // namespace

EffectPoolBank::EffectPoolBank(const EngineConfig& config) {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    for (std::uint32_t r = 0; r < kEffectRegistryRowCount; ++r) {
        const auto& registry = kEffectRegistry[r];
        auto& slot = m_slots[r];
        slot.effectType = registry.effectType;
        slot.configuredLimit = registry.capacity(config);
        slot.strideUnits = StrideUnits(registry.stateSize);
        slot.registry = &registry;
        slot.sampleRate = config.mixRate.SampleRate();
        slot.extraFramesPerChannel = registry.extraFramesPerChannel(config);
    }
}

EffectPoolBank::~EffectPoolBank() {
    for (const auto& slot : m_slots)
        assert(slot.backingGrowth.heldRows == 0 && slot.backingGrowth.preparedUsers == 0);
}

bool EffectPoolBank::GrowBacking(PoolTypeSlot& slot, std::uint32_t count) noexcept {
    if (count <= slot.readyCount) return true;
    if (memguard::IsAudioTagged() || slot.backingGrowth.preparedUsers != 0) return false;
    const auto sampleCount = static_cast<std::uint64_t>(slot.extraFramesPerChannel) * kMaxChannels;
    if (sampleCount > static_cast<std::size_t>(PTRDIFF_MAX) / sizeof(float)) return false;
    try {
        if (count > slot.freeList.capacity()) {
            detail::LogicAllocCheckpoint(detail::LogicAllocSite::EffectInstanceStorage);
            const auto reserve = std::max<std::uint64_t>(count,
                std::min<std::uint64_t>(UINT32_MAX, slot.freeList.capacity() * 2ull));
            slot.freeList.reserve(static_cast<std::size_t>(reserve));
        }
        if (!slot.rows.ConstructThrough(count)) return false;
        while (slot.readyCount < count) {
            const auto index = slot.readyCount;
            auto& row = slot.rows[index];
            const auto laneIndex = std::bit_width(static_cast<std::uint64_t>(index) + 1u) - 1u;
            auto& lane = slot.retireLanes[laneIndex];
            if (!lane) {
                detail::LogicAllocCheckpoint(detail::LogicAllocSite::EffectRetireStorage);
                lane = std::make_unique<EffectRetireQueue>(UINT32_C(1) << laneIndex);
            }
            if (row.storage.empty()) {
                detail::LogicAllocCheckpoint(detail::LogicAllocSite::EffectInstanceStorage);
                row.storage.assign(slot.strideUnits, std::max_align_t{});
            }
            if (sampleCount != 0 && row.extra.empty()) {
                detail::LogicAllocCheckpoint(detail::LogicAllocSite::EffectExtraStorage);
                row.extra.assign(static_cast<std::size_t>(sampleCount), 0.0f);
            }
            if (slot.effectType == VN_EFFECT_COMPRESSOR && !row.compressorRuntime) {
                detail::LogicAllocCheckpoint(detail::LogicAllocSite::EffectInstanceStorage);
                row.compressorRuntime = std::make_unique<CompressorRuntimeSlot>();
            }
            slot.registry->initInstance(row.storage.data(),
                row.extra.empty() ? nullptr : row.extra.data(),
                slot.extraFramesPerChannel, slot.sampleRate);
            row.retireLane = lane.get();
            // Metadata alone is not an issuable instance. Publish availability
            // only after every dependency is ready, retaining caches on failure.
            slot.freeList.push_back(index);
            ++slot.readyCount;
        }
        return true;
    } catch (...) { return false; }
}

bool EffectPoolBank::EnsureBacking(PoolTypeSlot& slot, std::uint64_t count) noexcept {
    if (memguard::IsAudioTagged() || slot.backingGrowth.preparedUsers != 0) return false;
    const auto occupied = slot.OccupiedRows();
    const auto held = slot.backingGrowth.heldRows;
    std::uint64_t target = 0;
    if (slot.configuredLimit != 0) {
        // Explicit quotas saturate physical preparation, not logical admission.
        target = std::min<std::uint64_t>(slot.configuredLimit,
            occupied + std::min<std::uint64_t>(held, slot.configuredLimit) +
            std::min<std::uint64_t>(count, slot.configuredLimit));
    } else {
        if (count > UINT32_MAX || held > UINT32_MAX - count ||
            occupied > UINT32_MAX - count - held) return false;
        target = occupied + held + count;
    }
    return GrowBacking(slot, static_cast<std::uint32_t>(target));
}

bool EffectPoolBank::CanAdmit(std::uint32_t type, std::uint64_t count) const noexcept {
    const auto* slot = SlotFor(type);
    if (slot == nullptr) return false;
    if (slot->backingGrowth.preparedUsers != 0) return count <= slot->freeList.size();
    const auto limit = slot->configuredLimit == 0 ? UINT32_MAX : slot->configuredLimit;
    const auto occupied = slot->OccupiedRows();
    return occupied <= limit && count <= limit - occupied;
}

bool EffectPoolBank::EnsureAvailable(std::uint32_t type, std::uint64_t count) noexcept {
    auto* slot = SlotFor(type);
    if (slot == nullptr || !CanAdmit(type, count)) return false;
    if (slot->backingGrowth.preparedUsers != 0) return true;
    return EnsureBacking(*slot, count);
}

bool EffectPoolBank::PrepareBacking(const BackingDemand& demand, BackingPreparation* out) noexcept {
    if (out == nullptr || memguard::IsAudioTagged()) return false;
    for (const auto& column : *out) if (column.Valid()) return false;
    BackingPreparation prepared;
    for (std::uint32_t type = 0; type < m_slots.size(); ++type) {
        auto& slot = m_slots[type];
        const auto count = slot.configuredLimit != 0
            ? std::min<std::uint64_t>(demand[type], slot.configuredLimit) : demand[type];
        if (count > UINT32_MAX || slot.backingGrowth.heldRows > UINT64_MAX - count ||
            !EnsureBacking(slot, count)) return false;
        slot.backingGrowth.heldRows += count;
        prepared[type].state_ = &slot.backingGrowth;
        prepared[type].count_ = static_cast<std::uint32_t>(count);
    }
    *out = std::move(prepared);
    return true;
}

EffectPoolBank::BackingUse EffectPoolBank::UseBacking(const BackingPreparation* backing) noexcept {
    return [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        return BackingUse{(backing == nullptr ? StableStoragePreparedUse{} : (*backing)[Index].Use())...};
    }(std::make_index_sequence<kEffectRegistryRowCount>{});
}

EffectPoolBank::PreparedCommitProof::PreparedCommitProof(
    PreparedCommitProof&& other) noexcept
    : m_slot(std::exchange(other.m_slot, nullptr)),
      m_state(std::exchange(other.m_state, nullptr)),
      m_ref(std::exchange(other.m_ref, {})) {}

EffectPoolBank::PreparedCommitProof&
EffectPoolBank::PreparedCommitProof::operator=(PreparedCommitProof&& other) noexcept {
    if (this != &other) {
        Release();
        m_slot = std::exchange(other.m_slot, nullptr);
        m_state = std::exchange(other.m_state, nullptr);
        m_ref = std::exchange(other.m_ref, {});
    }
    return *this;
}

void EffectPoolBank::PreparedCommitProof::Release() {
    if (Valid()) {
        m_state->store(InstanceState::Prepared, std::memory_order_release);
    }
    m_slot = nullptr;
    m_state = nullptr;
    m_ref = {};
}

void EffectPoolBank::PreparedCommitProof::Consume() && {
    if (!Valid()) { return; }
    if (m_slot->inUse > m_slot->highWater) {
        m_slot->highWater = m_slot->inUse;
    }
    m_state->store(InstanceState::Live, std::memory_order_release);
    m_slot = nullptr;
    m_state = nullptr;
    m_ref = {};
}

EffectInstanceRef EffectPoolBank::AcquireWithState(
    std::uint32_t type, EffectObjectIdentity identity, InstanceState state) {
    PoolTypeSlot* slot = SlotFor(type);
    if (slot == nullptr) { return {}; }
    if (!EnsureAvailable(type, 1)) {
        ++slot->refusalCount;
        return {};
    }
    const auto idx = slot->freeList.back();
    slot->freeList.pop_back();
    // Only genuinely Free rows enter this freelist. A maximum generation is
    // issued once, then disposal quarantines the row permanently.
    const std::uint64_t generation = ++slot->rows[idx].generation;
    ++slot->inUse;
    slot->rows[idx].identity = identity;
    slot->rows[idx].installed = 0;
    if (state == InstanceState::Live && slot->inUse > slot->highWater) {
        slot->highWater = slot->inUse;
    }
    slot->rows[idx].state.store(state, std::memory_order_release);
    return EffectInstanceRef{type, idx, generation};
}

EffectInstanceRef EffectPoolBank::Acquire(
    std::uint32_t type, EffectObjectIdentity identity) {
    return AcquireWithState(type, identity, InstanceState::Live);
}

EffectInstanceRef EffectPoolBank::AcquirePrepared(
    std::uint32_t type, EffectObjectIdentity identity) {
    return AcquireWithState(type, identity, InstanceState::Prepared);
}

bool EffectPoolBank::ProvePreparedCommit(
    const EffectInstanceRef& original, EffectObjectIdentity originalIdentity,
    PreparedCommitProof* out) {
    if (out == nullptr || out->Valid()) { return false; }
    PoolTypeSlot* slot = SlotFor(original.type);
    if (slot == nullptr || original.poolIndex >= slot->readyCount ||
        original.generation == 0) { return false; }
    const std::uint32_t index = original.poolIndex;
    if (slot->rows[index].state.load(std::memory_order_acquire) != InstanceState::Prepared ||
        slot->rows[index].generation != original.generation || slot->rows[index].installed != 0 ||
        slot->rows[index].identity.effectId != originalIdentity.effectId ||
        slot->rows[index].identity.projectId != originalIdentity.projectId) { return false; }
    InstanceState expected = InstanceState::Prepared;
    if (!slot->rows[index].state.compare_exchange_strong(
            expected, InstanceState::PreparedCommitHeld,
            std::memory_order_acq_rel, std::memory_order_acquire)) { return false; }
    if (slot->rows[index].generation != original.generation || slot->rows[index].installed != 0 ||
        slot->rows[index].identity.effectId != originalIdentity.effectId ||
        slot->rows[index].identity.projectId != originalIdentity.projectId) {
        slot->rows[index].state.store(InstanceState::Prepared, std::memory_order_release);
        return false;
    }
    out->m_slot = slot;
    out->m_state = &slot->rows[index].state;
    out->m_ref = original;
    return true;
}

bool EffectPoolBank::CaptureOriginalPreparedEffect(const PreparedCommitProof& proof,
    EffectInstanceRef* out) const noexcept {
    if (!proof.Valid() || out == nullptr ||
        proof.m_state->load(std::memory_order_acquire) != InstanceState::PreparedCommitHeld)
        return false;
    if (SlotFor(proof.m_ref.type) != proof.m_slot) return false;
    *out = proof.m_ref;
    return true;
}

bool EffectPoolBank::CommitPrepared(const EffectInstanceRef& ref) {
    PoolTypeSlot* slot = SlotFor(ref.type);
    if (slot == nullptr || ref.poolIndex >= slot->readyCount || ref.generation == 0 ||
        slot->rows[ref.poolIndex].state.load(std::memory_order_acquire) != InstanceState::Prepared ||
        slot->rows[ref.poolIndex].generation != ref.generation) { return false; }
    if (slot->inUse > slot->highWater) { slot->highWater = slot->inUse; }
    slot->rows[ref.poolIndex].state.store(InstanceState::Live, std::memory_order_release);
    return true;
}

bool EffectPoolBank::CancelPrepared(const EffectInstanceRef& ref) {
    PoolTypeSlot* slot = SlotFor(ref.type);
    if (slot == nullptr || ref.poolIndex >= slot->readyCount || ref.generation == 0 ||
        slot->rows[ref.poolIndex].state.load(std::memory_order_acquire) != InstanceState::Prepared ||
        slot->rows[ref.poolIndex].generation != ref.generation || slot->inUse == 0) { return false; }
    Dispose(*slot, ref);
    return true;
}

void EffectPoolBank::Dispose(PoolTypeSlot& slot, const EffectInstanceRef& ref) {
    slot.rows[ref.poolIndex].identity = {};
    slot.rows[ref.poolIndex].installed = 0;
    --slot.inUse;
    if (ref.generation == std::numeric_limits<std::uint64_t>::max()) {
        slot.rows[ref.poolIndex].state.store(InstanceState::Exhausted, std::memory_order_release);
    } else {
        slot.rows[ref.poolIndex].state.store(InstanceState::Free, std::memory_order_release);
        // Growth reserves freelist space before issuing any row.
        slot.freeList.push_back(ref.poolIndex);
    }
}

void EffectPoolBank::Recycle(const EffectInstanceRef& ref) {
    PoolTypeSlot* slot = SlotFor(ref.type);
    if (slot == nullptr || ref.poolIndex >= slot->readyCount || ref.generation == 0) { return; }
    const InstanceState state = slot->rows[ref.poolIndex].state.load(std::memory_order_acquire);
    if ((state != InstanceState::Live && state != InstanceState::Prepared && state != InstanceState::Retired) ||
        slot->rows[ref.poolIndex].generation != ref.generation || slot->inUse == 0) { return; }
    if (slot->rows[ref.poolIndex].installed != 0) {
        const EffectObjectIdentity identity = slot->rows[ref.poolIndex].identity;
        MonitorLifecycleFacts facts{};
        facts.projectId = identity.projectId;
        ObserveMonitorLifecycle(MonitorLifecyclePhase::EffectReleased,
            (static_cast<std::uint64_t>(ref.type + 1u) << 32) | (ref.poolIndex + 1u),
            ref.generation, 0, identity.effectId, 0, 0,
            VN_ACTION_TERMINAL_NONE, 0, 0, facts);
    }
    if (ref.type == VN_EFFECT_COMPRESSOR) {
        auto* state = static_cast<CompressorState*>(slot->StateAt(ref.poolIndex));
        const std::uint32_t sampleRate = state->sampleRate;
        CompressorInit(state, sampleRate);
        if (slot->rows[ref.poolIndex].compressorRuntime != nullptr) {
            slot->rows[ref.poolIndex].compressorRuntime->lastCoherent = CompressorRuntimePayload{};
        }
    }
    Dispose(*slot, ref);
}

bool EffectPoolBank::Retire(const EffectInstanceRef& ref) noexcept {
    auto* slot = SlotFor(ref.type);
    auto* row = slot == nullptr ? nullptr : slot->rows.TryAt(ref.poolIndex);
    if (row == nullptr || ref.generation == 0 || ref.generation <= row->retiredGeneration)
        return true;
    if (row->state.load(std::memory_order_acquire) != InstanceState::Live ||
        row->generation.load(std::memory_order_acquire) != ref.generation) return true;
    // Only the mixer owns published instances. An earlier retirement is caught
    // above before logic can recycle the row into a new Live generation.
    const auto previous = row->retiredGeneration;
    row->state.store(InstanceState::Retired, std::memory_order_release);
    row->retiredGeneration = ref.generation;
    if (row->retireLane->TryPush(ref)) return true;
    // A lane holds every row assigned to it; full implies a broken invariant.
    row->retiredGeneration = previous;
    row->state.store(InstanceState::Live, std::memory_order_release);
    return false;
}

std::uint64_t EffectPoolBank::DrainRetire() {
    EffectInstanceRef ref;
    std::uint64_t count = 0;
    for (auto& slot : m_slots)
        for (const auto& lane : slot.retireLanes)
            if (lane) while (lane->TryPop(ref)) { Recycle(ref); ++count; }
    return count;
}

EffectObjectIdentity EffectPoolBank::ObjectIdentity(const EffectInstanceRef& ref) const {
    const PoolTypeSlot* slot = SlotFor(ref.type);
    if (slot == nullptr || ref.poolIndex >= slot->readyCount || ref.generation == 0 ||
        slot->rows[ref.poolIndex].generation != ref.generation) return {};
    const InstanceState state = slot->rows[ref.poolIndex].state.load(std::memory_order_acquire);
    if (state != InstanceState::Prepared && state != InstanceState::PreparedCommitHeld &&
        state != InstanceState::Live) return {};
    return slot->rows[ref.poolIndex].identity;
}

void EffectPoolBank::PublishInstalled(const EffectInstanceRef& ref) {
    PoolTypeSlot* slot = SlotFor(ref.type);
    if (slot == nullptr || ref.poolIndex >= slot->readyCount || ref.generation == 0 ||
        slot->rows[ref.poolIndex].generation != ref.generation ||
        slot->rows[ref.poolIndex].state.load(std::memory_order_acquire) != InstanceState::Live ||
        slot->rows[ref.poolIndex].installed != 0) return;
    slot->rows[ref.poolIndex].installed = 1;
    const EffectObjectIdentity identity = slot->rows[ref.poolIndex].identity;
    MonitorLifecycleFacts facts{};
    facts.projectId = identity.projectId;
    ObserveMonitorLifecycle(MonitorLifecyclePhase::EffectInstalled,
        (static_cast<std::uint64_t>(ref.type + 1u) << 32) | (ref.poolIndex + 1u),
        ref.generation, 0, identity.effectId, 0, 0,
        VN_ACTION_TERMINAL_NONE, 0, 0, facts);
}

void* EffectPoolBank::StateFor(std::uint32_t type, std::uint32_t index) {
    auto* slot = SlotFor(type);
    auto* row = slot == nullptr ? nullptr : slot->rows.TryAt(index);
    if (row == nullptr) return nullptr;
    const auto state = row->state.load(std::memory_order_acquire);
    if (state != InstanceState::Prepared && state != InstanceState::Live) return nullptr;
    return row->storage.data();
}

void* EffectPoolBank::LiveStateFor(const EffectInstanceRef& ref) {
    auto* slot = SlotFor(ref.type);
    auto* row = slot == nullptr ? nullptr : slot->rows.TryAt(ref.poolIndex);
    if (row == nullptr || ref.generation == 0 ||
        row->state.load(std::memory_order_acquire) != InstanceState::Live ||
        row->generation.load(std::memory_order_acquire) != ref.generation) return nullptr;
    return row->storage.data();
}

void EffectPoolBank::PublishCompressorRuntime(
    const EffectInstanceRef& ref,
    const CompressorRuntimePayload& payload) {
    PoolTypeSlot* slot = SlotFor(VN_EFFECT_COMPRESSOR);
    auto* row = slot == nullptr ? nullptr : slot->rows.TryAt(ref.poolIndex);
    if (row == nullptr || ref.type != VN_EFFECT_COMPRESSOR || ref.generation == 0 ||
        row->state.load(std::memory_order_acquire) != InstanceState::Live ||
        row->generation.load(std::memory_order_acquire) != ref.generation ||
        row->compressorRuntime == nullptr) {
        return;
    }
    CompressorRuntimePayload& write =
        row->compressorRuntime->published.BeginWrite();
    write = payload;
    write.poolGeneration = ref.generation;
    row->compressorRuntime->published.EndWrite();
    m_audioCompressorDetectorNonFiniteCount +=
        payload.detectorNonFiniteCount;
    m_audioCompressorDetectorSaturationCount +=
        payload.detectorSaturationCount;
}

bool EffectPoolBank::CaptureCompressorRuntime(
    const EffectInstanceRef& ref,
    CompressorRuntimePayload* out) {
    if (out == nullptr) {
        return false;
    }
    PoolTypeSlot* slot = SlotFor(VN_EFFECT_COMPRESSOR);
    auto* row = slot == nullptr ? nullptr : slot->rows.TryAt(ref.poolIndex);
    if (row == nullptr || ref.type != VN_EFFECT_COMPRESSOR || ref.generation == 0 ||
        row->state.load(std::memory_order_acquire) != InstanceState::Live ||
        row->generation.load(std::memory_order_acquire) != ref.generation ||
        row->compressorRuntime == nullptr) {
        *out = CompressorRuntimePayload{};
        return false;
    }
    CompressorRuntimeSlot& runtime = *row->compressorRuntime;
    CompressorRuntimePayload captured = runtime.lastCoherent;
    const bool coherent = runtime.published.Capture(captured);
    if (coherent) {
        runtime.lastCoherent = captured;
    }
    *out = captured;
    return coherent;
}

} // namespace harken
