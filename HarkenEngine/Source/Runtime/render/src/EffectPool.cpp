#include "harken/render/EffectPool.h"
#include "harken/core/MonitorLifecycleSink.h"

#include <cstddef>
#include <limits>
#include <utility>

namespace harken {

namespace {

// Total instance count across every registered type, from the generated
// registry rows and the resolved config. Sizes the retire ring so it can hold
// every instance at once (an instance is on at most one slot, so outstanding
// retirements never exceed this).
std::uint32_t TotalInstanceCapacity(const EngineConfig& config) {
    std::uint32_t total = 0;
    for (std::uint32_t r = 0; r < kEffectRegistryRowCount; ++r) {
        total += kEffectRegistry[r].capacity(config);
    }
    return total;
}

// Whole std::max_align_t units needed to hold one instance of `stateBytes`. Each
// instance occupies whole units, so its base address is max_align_t-aligned —
// which the registry pins to be at least every state type's alignment.
std::uint32_t StrideUnits(std::uint32_t stateBytes) {
    const std::uint32_t unit =
        static_cast<std::uint32_t>(sizeof(std::max_align_t));
    return (stateBytes + unit - 1u) / unit;
}

} // namespace

EffectPoolBank::EffectPoolBank(const EngineConfig& config)
    : m_retire(TotalInstanceCapacity(config)) {
    m_slots.resize(kEffectRegistryRowCount);
    for (std::uint32_t r = 0; r < kEffectRegistryRowCount; ++r) {
        const EffectRegistryRow& row = kEffectRegistry[r];
        PoolTypeSlot& slot = m_slots[r];
        slot.effectType = row.effectType;
        slot.capacity = row.capacity(config);
        slot.strideUnits = StrideUnits(row.stateSize);

        // Type-erased instance storage: capacity * strideUnits max_align_t units,
        // zero-filled before construction. Every instance base is aligned to
        // alignof(std::max_align_t) (>= the state's alignment, pinned by the
        // registry static_asserts).
        slot.storage.assign(static_cast<std::size_t>(slot.capacity) *
                                slot.strideUnits,
                            std::max_align_t{});

        // Per-instance extra storage (the delay line): frames per channel across
        // kMaxChannels, carved per instance. Zero for effects that request none.
        const std::uint32_t extraFrames = row.extraFramesPerChannel(config);
        const std::size_t extraPerInstance =
            static_cast<std::size_t>(extraFrames) * kMaxChannels;
        slot.extra.assign(static_cast<std::size_t>(slot.capacity) *
                              extraPerInstance,
                          0.0f);

        slot.freeList.reserve(slot.capacity);
        slot.generations.assign(slot.capacity, 0);
        slot.states = std::make_unique<std::atomic<InstanceState>[]>(slot.capacity);
        for (std::uint32_t i = 0; i < slot.capacity; ++i) {
            slot.states[i].store(InstanceState::Free, std::memory_order_relaxed);
        }
        slot.identities.resize(slot.capacity);
        slot.installed.assign(slot.capacity, 0);
        if (row.effectType == VN_EFFECT_COMPRESSOR) {
            slot.compressorRuntime =
                std::make_unique<
                    EffectPoolBank::PoolTypeSlot::CompressorRuntimeSlot[]>(
                    slot.capacity);
        }
        for (std::uint32_t i = 0; i < slot.capacity; ++i) {
            float* extraBase =
                extraPerInstance != 0
                    ? slot.extra.data() + static_cast<std::size_t>(i) *
                                              extraPerInstance
                    : nullptr;
            row.initInstance(slot.StateAt(i), extraBase, extraFrames,
                             config.mixRate.SampleRate());
            // Push in reverse so the first acquisitions hand out ascending
            // indices (0, 1, 2, ...) — friendlier to read in snapshots and
            // tests.
            slot.freeList.push_back(slot.capacity - 1 - i);
        }
    }
}

EffectPoolBank::PreparedCommitProof::PreparedCommitProof(
    PreparedCommitProof&& other) noexcept
    : m_slot(std::exchange(other.m_slot, nullptr)),
      m_state(std::exchange(other.m_state, nullptr)) {}

EffectPoolBank::PreparedCommitProof&
EffectPoolBank::PreparedCommitProof::operator=(PreparedCommitProof&& other) noexcept {
    if (this != &other) {
        Release();
        m_slot = std::exchange(other.m_slot, nullptr);
        m_state = std::exchange(other.m_state, nullptr);
    }
    return *this;
}

void EffectPoolBank::PreparedCommitProof::Release() {
    if (Valid()) {
        m_state->store(InstanceState::Prepared, std::memory_order_release);
    }
    m_slot = nullptr;
    m_state = nullptr;
}

void EffectPoolBank::PreparedCommitProof::Consume() && {
    if (!Valid()) { return; }
    if (m_slot->inUse > m_slot->highWater) {
        m_slot->highWater = m_slot->inUse;
    }
    m_state->store(InstanceState::Live, std::memory_order_release);
    m_slot = nullptr;
    m_state = nullptr;
}

EffectInstanceRef EffectPoolBank::AcquireWithState(
    std::uint32_t type, EffectObjectIdentity identity, InstanceState state) {
    PoolTypeSlot* slot = SlotFor(type);
    if (slot == nullptr) { return {}; }
    if (slot->freeList.empty()) {
        ++slot->refusalCount;
        return {};
    }
    const std::uint32_t idx = slot->freeList.back();
    slot->freeList.pop_back();
    // Only genuinely Free rows enter this freelist. A maximum generation is
    // issued once, then disposal quarantines the row permanently.
    const std::uint64_t generation = ++slot->generations[idx];
    ++slot->inUse;
    slot->identities[idx] = identity;
    slot->installed[idx] = 0;
    if (state == InstanceState::Live && slot->inUse > slot->highWater) {
        slot->highWater = slot->inUse;
    }
    slot->states[idx].store(state, std::memory_order_release);
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
    if (slot == nullptr || original.poolIndex >= slot->capacity ||
        original.generation == 0) { return false; }
    const std::uint32_t index = original.poolIndex;
    if (slot->states[index].load(std::memory_order_acquire) != InstanceState::Prepared ||
        slot->generations[index] != original.generation || slot->installed[index] != 0 ||
        slot->identities[index].effectId != originalIdentity.effectId ||
        slot->identities[index].projectId != originalIdentity.projectId) { return false; }
    InstanceState expected = InstanceState::Prepared;
    if (!slot->states[index].compare_exchange_strong(
            expected, InstanceState::PreparedCommitHeld,
            std::memory_order_acq_rel, std::memory_order_acquire)) { return false; }
    if (slot->generations[index] != original.generation || slot->installed[index] != 0 ||
        slot->identities[index].effectId != originalIdentity.effectId ||
        slot->identities[index].projectId != originalIdentity.projectId) {
        slot->states[index].store(InstanceState::Prepared, std::memory_order_release);
        return false;
    }
    out->m_slot = slot;
    out->m_state = &slot->states[index];
    return true;
}

bool EffectPoolBank::CaptureOriginalPreparedEffect(const PreparedCommitProof& proof,
    EffectInstanceRef* out) const noexcept {
    if (!proof.Valid() || out == nullptr ||
        proof.m_state->load(std::memory_order_acquire) != InstanceState::PreparedCommitHeld)
        return false;
    for (const auto& slot : m_slots) {
        if (&slot != proof.m_slot) continue;
        const auto index = static_cast<std::uint32_t>(proof.m_state - slot.states.get());
        if (index >= slot.capacity) return false;
        *out = {slot.effectType, index, slot.generations[index]};
        return true;
    }
    return false;
}

bool EffectPoolBank::CommitPrepared(const EffectInstanceRef& ref) {
    PoolTypeSlot* slot = SlotFor(ref.type);
    if (slot == nullptr || ref.poolIndex >= slot->capacity || ref.generation == 0 ||
        slot->states[ref.poolIndex].load(std::memory_order_acquire) != InstanceState::Prepared ||
        slot->generations[ref.poolIndex] != ref.generation) { return false; }
    if (slot->inUse > slot->highWater) { slot->highWater = slot->inUse; }
    slot->states[ref.poolIndex].store(InstanceState::Live, std::memory_order_release);
    return true;
}

bool EffectPoolBank::CancelPrepared(const EffectInstanceRef& ref) {
    PoolTypeSlot* slot = SlotFor(ref.type);
    if (slot == nullptr || ref.poolIndex >= slot->capacity || ref.generation == 0 ||
        slot->states[ref.poolIndex].load(std::memory_order_acquire) != InstanceState::Prepared ||
        slot->generations[ref.poolIndex] != ref.generation || slot->inUse == 0) { return false; }
    Dispose(*slot, ref);
    return true;
}

void EffectPoolBank::Dispose(PoolTypeSlot& slot, const EffectInstanceRef& ref) {
    slot.identities[ref.poolIndex] = {};
    slot.installed[ref.poolIndex] = 0;
    --slot.inUse;
    if (ref.generation == std::numeric_limits<std::uint64_t>::max()) {
        slot.states[ref.poolIndex].store(InstanceState::Exhausted, std::memory_order_release);
    } else {
        slot.states[ref.poolIndex].store(InstanceState::Free, std::memory_order_release);
        // Init reserved the full capacity; a row can be disposed only once.
        slot.freeList.push_back(ref.poolIndex);
    }
}

void EffectPoolBank::Recycle(const EffectInstanceRef& ref) {
    PoolTypeSlot* slot = SlotFor(ref.type);
    if (slot == nullptr || ref.poolIndex >= slot->capacity || ref.generation == 0) { return; }
    const InstanceState state = slot->states[ref.poolIndex].load(std::memory_order_acquire);
    if ((state != InstanceState::Live && state != InstanceState::Prepared) ||
        slot->generations[ref.poolIndex] != ref.generation || slot->inUse == 0) { return; }
    if (slot->installed[ref.poolIndex] != 0) {
        const EffectObjectIdentity identity = slot->identities[ref.poolIndex];
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
        if (slot->compressorRuntime != nullptr) {
            slot->compressorRuntime[ref.poolIndex].lastCoherent = CompressorRuntimePayload{};
        }
    }
    Dispose(*slot, ref);
}

void EffectPoolBank::DrainRetire() {
    EffectInstanceRef ref;
    while (m_retire.TryPop(ref)) {
        Recycle(ref);
    }
}

EffectObjectIdentity EffectPoolBank::ObjectIdentity(const EffectInstanceRef& ref) const {
    const PoolTypeSlot* slot = SlotFor(ref.type);
    if (slot == nullptr || ref.poolIndex >= slot->capacity || ref.generation == 0 ||
        slot->generations[ref.poolIndex] != ref.generation) return {};
    const InstanceState state = slot->states[ref.poolIndex].load(std::memory_order_acquire);
    if (state != InstanceState::Prepared && state != InstanceState::PreparedCommitHeld &&
        state != InstanceState::Live) return {};
    return slot->identities[ref.poolIndex];
}

void EffectPoolBank::PublishInstalled(const EffectInstanceRef& ref) {
    PoolTypeSlot* slot = SlotFor(ref.type);
    if (slot == nullptr || ref.poolIndex >= slot->capacity || ref.generation == 0 ||
        slot->generations[ref.poolIndex] != ref.generation ||
        slot->states[ref.poolIndex].load(std::memory_order_acquire) != InstanceState::Live ||
        slot->installed[ref.poolIndex] != 0) return;
    slot->installed[ref.poolIndex] = 1;
    const EffectObjectIdentity identity = slot->identities[ref.poolIndex];
    MonitorLifecycleFacts facts{};
    facts.projectId = identity.projectId;
    ObserveMonitorLifecycle(MonitorLifecyclePhase::EffectInstalled,
        (static_cast<std::uint64_t>(ref.type + 1u) << 32) | (ref.poolIndex + 1u),
        ref.generation, 0, identity.effectId, 0, 0,
        VN_ACTION_TERMINAL_NONE, 0, 0, facts);
}

void* EffectPoolBank::StateFor(std::uint32_t type, std::uint32_t index) {
    PoolTypeSlot* slot = SlotFor(type);
    if (slot == nullptr || index >= slot->capacity) {
        return nullptr;
    }
    const InstanceState state = slot->states[index].load(std::memory_order_acquire);
    if (state != InstanceState::Prepared && state != InstanceState::Live) { return nullptr; }
    return slot->StateAt(index);
}

void* EffectPoolBank::LiveStateFor(const EffectInstanceRef& ref) {
    PoolTypeSlot* slot = SlotFor(ref.type);
    if (slot == nullptr || ref.poolIndex >= slot->capacity || ref.generation == 0 ||
        slot->states[ref.poolIndex].load(std::memory_order_acquire) != InstanceState::Live ||
        slot->generations[ref.poolIndex] != ref.generation) { return nullptr; }
    return slot->StateAt(ref.poolIndex);
}

void EffectPoolBank::PublishCompressorRuntime(
    const EffectInstanceRef& ref,
    const CompressorRuntimePayload& payload) {
    PoolTypeSlot* slot = SlotFor(VN_EFFECT_COMPRESSOR);
    if (slot == nullptr || ref.type != VN_EFFECT_COMPRESSOR ||
        ref.poolIndex >= slot->capacity ||
        slot->compressorRuntime == nullptr ||
        ref.generation == 0 ||
        slot->states[ref.poolIndex].load(std::memory_order_acquire) != InstanceState::Live ||
        slot->generations[ref.poolIndex] != ref.generation) {
        return;
    }
    CompressorRuntimePayload& write =
        slot->compressorRuntime[ref.poolIndex].published.BeginWrite();
    write = payload;
    write.poolGeneration = ref.generation;
    slot->compressorRuntime[ref.poolIndex].published.EndWrite();
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
    if (slot == nullptr || ref.type != VN_EFFECT_COMPRESSOR ||
        ref.poolIndex >= slot->capacity ||
        slot->compressorRuntime == nullptr ||
        ref.generation == 0 ||
        slot->states[ref.poolIndex].load(std::memory_order_acquire) != InstanceState::Live ||
        slot->generations[ref.poolIndex] != ref.generation) {
        *out = CompressorRuntimePayload{};
        return false;
    }
    PoolTypeSlot::CompressorRuntimeSlot& runtime =
        slot->compressorRuntime[ref.poolIndex];
    CompressorRuntimePayload captured = runtime.lastCoherent;
    const bool coherent = runtime.published.Capture(captured);
    if (coherent) {
        runtime.lastCoherent = captured;
    }
    *out = captured;
    return coherent;
}

} // namespace harken
