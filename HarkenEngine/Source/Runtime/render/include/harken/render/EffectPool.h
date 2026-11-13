// Logic-owned backing grows before publication; audio only runs and retires
// stable instances. Zero configured capacity grows on demand, nonzero is a quota.
#ifndef HARKEN_RENDER_EFFECT_POOL_H
#define HARKEN_RENDER_EFFECT_POOL_H

#include <harken/Harken.h>

#include "harken/core/EngineConfig.h"
#include "harken/core/PinnedSnapshot3.h"
#include "harken/core/GrowingRowStorage.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/InsertEffect.h"
#include "harken/render/generated/harken_effect_registry.g.h"

#include <atomic>
#include <array>
#include <bit>
#include <limits>
#include <new>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace harken {

struct EffectObjectIdentity {
    VnObjectId effectId{};
    VnObjectId projectId{};
};

struct CompressorRuntimePayload {
    std::uint64_t poolGeneration = 0;
    VnObjectId hostObjectId{};
    std::uint64_t hostIdentity = 0;
    std::uint64_t blockEpoch = 0;
    std::uint64_t detectorNonFiniteCount = 0;
    std::uint64_t detectorSaturationCount = 0;
    std::uint32_t hostScope = 0;
    std::uint32_t hostDenseIndex = 0;
    std::uint32_t effectSlot = 0;
    std::uint32_t stateFlags = 0;
    std::uint32_t transitionFramesRemaining = 0;
    std::uint32_t staleRouteCount = 0;
    float mainInputPeakLinear = 0.0f;
    float detectorPeakLinear = 0.0f;
    float outputPeakLinear = 0.0f;
    float maxGainReductionDb = 0.0f;
    float endGainReductionDb = 0.0f;
};

// One immutable audio-to-logic SPSC lane. Its ring holds every row assigned to
// that geometric index range; each generation is queued at most once and a
// row cannot be reissued until logic consumes its retirement. The mixer is
// the sole producer; logic drains the lane and returns rows to the freelist.
class EffectRetireQueue {
public:
    explicit EffectRetireQueue(std::uint32_t capacityHint) {
        if (capacityHint > (UINT32_C(1) << 31)) throw std::bad_alloc{};
        const auto cap = std::max(8u, std::bit_ceil(std::max(1u, capacityHint)));
        if (static_cast<std::size_t>(cap) > static_cast<std::size_t>(PTRDIFF_MAX) / sizeof(EffectInstanceRef))
            throw std::bad_alloc{};
        // Monotonic head/tail allow using every slot without a sentinel.
        m_slots.resize(cap);
        m_mask = cap - 1;
    }

    // Producer (mixer thread). Full indicates a violated sizing invariant.
    // EffectPoolBank restores Live on failure so retrying owners keep the ref.
    bool TryPush(const EffectInstanceRef& ref) {
        const std::uint64_t head = m_head.load(std::memory_order_relaxed);
        const std::uint64_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= m_slots.size()) {
            return false;
        }
        m_slots[head & m_mask] = ref;
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer (logic thread). Returns false when drained.
    bool TryPop(EffectInstanceRef& out) {
        const std::uint64_t tail = m_tail.load(std::memory_order_relaxed);
        const std::uint64_t head = m_head.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        out = m_slots[tail & m_mask];
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    std::vector<EffectInstanceRef> m_slots;
    std::uint32_t m_mask = 0;
    std::atomic<std::uint64_t> m_head{0}; // producer-owned (audio)
    std::atomic<std::uint64_t> m_tail{0}; // consumer-owned (logic)
};

// One stable pool per effect type, with independently growing SPSC retire lanes. Constructed once at
// Init; never moved afterwards (instance state holds pointers into this
// object's own storage).
namespace detail { struct EffectPoolTestAccess; }

class EffectPoolBank {
    enum class InstanceState : std::uint8_t {
        Free, Prepared, PreparedCommitHeld, Live, Retired, Exhausted
    };
    struct PoolTypeSlot;
public:
    using BackingDemand = std::array<std::uint64_t, kEffectRegistryRowCount>;
    using BackingPreparation = std::array<StableStoragePreparation, kEffectRegistryRowCount>;
    using BackingUse = std::array<StableStoragePreparedUse, kEffectRegistryRowCount>;

    bool PrepareBacking(const BackingDemand& demand, BackingPreparation* out) noexcept;
    static BackingUse UseBacking(const BackingPreparation* backing) noexcept;

    class PreparedCommitProof {
    public:
        PreparedCommitProof() = default;
        ~PreparedCommitProof() { Release(); }
        PreparedCommitProof(const PreparedCommitProof&) = delete;
        PreparedCommitProof& operator=(const PreparedCommitProof&) = delete;
        PreparedCommitProof(PreparedCommitProof&& other) noexcept;
        PreparedCommitProof& operator=(PreparedCommitProof&& other) noexcept;
        bool Valid() const { return m_slot != nullptr && m_state != nullptr; }
        void Release();
        void Consume() &&;
    private:
        friend class EffectPoolBank;
        PoolTypeSlot* m_slot = nullptr;
        std::atomic<InstanceState>* m_state = nullptr;
        EffectInstanceRef m_ref{};
    };
    explicit EffectPoolBank(const EngineConfig& config);
    ~EffectPoolBank();

    EffectPoolBank(const EffectPoolBank&) = delete;
    EffectPoolBank& operator=(const EffectPoolBank&) = delete;

    // --- logic thread ---

    // Takes a free instance of `type` and returns its reference, or a
    // kEffectTypeNone reference when that type's pool is exhausted. The
    // instance is NOT reset here; the caller resets and configures it before
    // handing it to the audio thread.
    EffectInstanceRef Acquire(std::uint32_t type, EffectObjectIdentity identity = {});
    EffectInstanceRef AcquirePrepared(std::uint32_t type, EffectObjectIdentity identity = {});
    bool ProvePreparedCommit(const EffectInstanceRef& original,
                             EffectObjectIdentity originalIdentity,
                             PreparedCommitProof* out);
    bool CaptureOriginalPreparedEffect(const PreparedCommitProof& proof,
        EffectInstanceRef* out) const noexcept;
    bool CommitPrepared(const EffectInstanceRef& ref);
    bool CancelPrepared(const EffectInstanceRef& ref);
    EffectObjectIdentity ObjectIdentity(const EffectInstanceRef& ref) const;
    void PublishInstalled(const EffectInstanceRef& ref);

    // Returns a retired instance to its pool's freelist. Bounds-checked and
    // ignores a kEffectTypeNone reference.
    void Recycle(const EffectInstanceRef& ref);

    // Drains every pending retirement into the freelists. Called at Set time
    // (to maximize free instances before acquiring) and each Vn_Update.
    std::uint64_t DrainRetire();

    // --- both threads (read-only, stable) ---

    // Resolves (type, index) to its instance state pointer. The address is
    // stable for the engine's life; the audio side uses it to build the
    // InsertEffect it installs, the logic side to reset/configure a fresh
    // instance. Non-const because the returned pointer is used to mutate
    // instance state (running the effect); the bank's own bookkeeping is
    // untouched.
    void* StateFor(std::uint32_t type, std::uint32_t index);
    void* LiveStateFor(const EffectInstanceRef& ref);

    void PublishCompressorRuntime(
        const EffectInstanceRef& ref,
        const CompressorRuntimePayload& payload);
    bool CaptureCompressorRuntime(
        const EffectInstanceRef& ref,
        CompressorRuntimePayload* out);
    std::uint64_t AudioCompressorDetectorNonFiniteCount() const {
        return m_audioCompressorDetectorNonFiniteCount;
    }
    std::uint64_t AudioCompressorDetectorSaturationCount() const {
        return m_audioCompressorDetectorSaturationCount;
    }

    // --- audio thread ---

    // Only the mixer owner retires published refs, after workers have joined.
    // Logic-only cancellation excludes audio owners and queued commands.
    bool Retire(const EffectInstanceRef& ref) noexcept;

    // --- observability (logic thread) ---
    // The per-type accessors keep their names and meaning; each resolves its
    // effect's registry-driven slot and reports that slot's field.

    std::uint32_t DelayCapacity() const {
        return CapacityOf(VN_EFFECT_FEEDBACK_DELAY);
    }
    std::uint32_t ToneCapacity() const {
        return CapacityOf(VN_EFFECT_THREE_BAND_TONE);
    }
    std::uint32_t DelayHighWater() const {
        return HighWaterOf(VN_EFFECT_FEEDBACK_DELAY);
    }
    std::uint32_t ToneHighWater() const {
        return HighWaterOf(VN_EFFECT_THREE_BAND_TONE);
    }
    std::uint32_t DelayInUse() const {
        return InUseOf(VN_EFFECT_FEEDBACK_DELAY);
    }
    std::uint32_t ToneInUse() const {
        return InUseOf(VN_EFFECT_THREE_BAND_TONE);
    }

    // Generic per-type observability, keyed by VnEffectType. The snapshot fill
    // walks the registry rows and reads these; the named accessors above are
    // thin wrappers for the legacy scalars. Unknown types report zero.
    // Explicit mode reports its quota; automatic mode reports fully backed rows,
    // including free/exhausted rows. This observation is not an admission limit.
    std::uint32_t CapacityOf(std::uint32_t type) const {
        const PoolTypeSlot* slot = SlotFor(type);
        return slot != nullptr ? (slot->configuredLimit != 0 ? slot->configuredLimit : slot->readyCount) : 0;
    }
    std::uint32_t ConfiguredLimitOf(std::uint32_t type) const {
        const auto* slot = SlotFor(type);
        return slot != nullptr ? slot->configuredLimit : 0;
    }
    bool CanAdmit(std::uint32_t type, std::uint64_t count) const noexcept;
    bool EnsureAvailable(std::uint32_t type, std::uint64_t count) noexcept;
    std::uint32_t HighWaterOf(std::uint32_t type) const {
        const PoolTypeSlot* slot = SlotFor(type);
        return slot != nullptr ? slot->highWater : 0;
    }
    std::uint32_t InUseOf(std::uint32_t type) const {
        const PoolTypeSlot* slot = SlotFor(type);
        return slot != nullptr ? slot->inUse : 0;
    }
    std::uint32_t RefusalCountOf(std::uint32_t type) const {
        const PoolTypeSlot* slot = SlotFor(type);
        return slot != nullptr ? slot->refusalCount : 0;
    }

private:
    friend struct detail::EffectPoolTestAccess;
    // One pooled effect type. State instances live in raw, over-aligned storage
    // (std::max_align_t units, each instance occupying strideUnits whole units so
    // its address satisfies any state alignment the registry allows); the extra
    // buffer holds per-instance storage the registry row requests. It is built
    // before issuing the instance and retained across reuse at a stable address.
    struct CompressorRuntimeSlot {
        PinnedSnapshot3<CompressorRuntimePayload> published;
        CompressorRuntimePayload lastCoherent{};
    };
    struct InstanceRow {
        std::atomic<InstanceState> state{InstanceState::Free};
        std::atomic<std::uint64_t> generation{0};
        // Mixer-owned, never cleared by logic when recycling this row.
        std::uint64_t retiredGeneration = 0;
        EffectObjectIdentity identity{};
        bool installed = false;
        std::vector<std::max_align_t> storage;
        std::vector<float> extra;
        std::unique_ptr<CompressorRuntimeSlot> compressorRuntime;
        EffectRetireQueue* retireLane = nullptr;
    };
    struct PoolTypeSlot {
        std::uint32_t effectType = kEffectTypeNone;
        std::uint32_t strideUnits = 0; // std::max_align_t units per instance
        std::uint32_t configuredLimit = 0;
        std::uint32_t readyCount = 0;
        const EffectRegistryRow* registry = nullptr;
        std::uint32_t extraFramesPerChannel = 0;
        std::uint32_t sampleRate = 0;
        GrowingRowStorage<InstanceRow> rows;
        StableStorageGrowthState backingGrowth;
        std::array<std::unique_ptr<EffectRetireQueue>, 32> retireLanes{};
        std::vector<std::uint32_t> freeList;
        std::uint32_t inUse = 0;
        std::uint32_t highWater = 0;
        std::uint32_t refusalCount = 0; // cumulative pool-exhaustion refusals

        void* StateAt(std::uint32_t index) {
            return rows[index].storage.data();
        }
        std::uint32_t OccupiedRows() const noexcept {
            return readyCount - static_cast<std::uint32_t>(freeList.size());
        }
    };

    PoolTypeSlot* SlotFor(std::uint32_t type) {
        for (PoolTypeSlot& slot : m_slots) {
            if (slot.effectType == type) {
                return &slot;
            }
        }
        return nullptr;
    }
    const PoolTypeSlot* SlotFor(std::uint32_t type) const {
        for (const PoolTypeSlot& slot : m_slots) {
            if (slot.effectType == type) {
                return &slot;
            }
        }
        return nullptr;
    }

    EffectInstanceRef AcquireWithState(std::uint32_t type,
                                       EffectObjectIdentity identity,
                                       InstanceState state);
    bool EnsureBacking(PoolTypeSlot& slot, std::uint64_t count) noexcept;
    bool GrowBacking(PoolTypeSlot& slot, std::uint32_t count) noexcept;
    void Dispose(PoolTypeSlot& slot, const EffectInstanceRef& ref);

    // One slot per generated registry row, in registry (effect-type) order.
    std::array<PoolTypeSlot, kEffectRegistryRowCount> m_slots{};
    // Audio-thread-owned cumulative totals. MixerLoop reads them only after
    // all per-host Compressor publications for the block have completed.
    std::uint64_t m_audioCompressorDetectorNonFiniteCount = 0;
    std::uint64_t m_audioCompressorDetectorSaturationCount = 0;
};

} // namespace harken

#endif // HARKEN_RENDER_EFFECT_POOL_H
