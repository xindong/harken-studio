// Effect instance pools and the audio->logic retirement channel realizing the
// M2 audio-thread contract amendment: effect instances are constructed
// logic-side from these preallocated pools at Set time and handed to the audio
// thread by command; the audio thread NEVER constructs or frees an effect. It
// only installs an already-built instance into a chain slot and, when that
// instance leaves a slot (voice end, clear, or in-place replace), names it on
// the retire queue for the logic side to return to its pool.
//
// One pool per effect type (fixed capacity from VnConfig, all storage — the
// delay lines included — preallocated at Init). Instance state pointers are
// resolved from the pool by (type, index): stable addresses into vectors that
// are sized once and never resized, so a pointer computed on the audio side
// equals the one the logic side sees. The freelist is touched only on the
// logic thread; index->pointer resolution is read-only and used on both.
//
// The per-type pools are not hand-written members; they are a bounded table
// with one row per built-in effect, sized and constructed from the generated
// effect registry (harken_effect_registry.g.h) at Init. Each row's per-instance
// state footprint, extra storage, and capacity all come from its registry row,
// so adding a built-in effect adds a registry row rather than another pool
// member here.
#ifndef HARKEN_RENDER_EFFECT_POOL_H
#define HARKEN_RENDER_EFFECT_POOL_H

#include <harken/Harken.h>

#include "harken/core/EngineConfig.h"
#include "harken/core/PinnedSnapshot3.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/InsertEffect.h"
#include "harken/render/generated/harken_effect_registry.g.h"

#include <atomic>
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

// Audio->logic single-producer single-consumer ring of retired instance
// references. Non-lossy: sized so it can hold every instance at once (an
// instance is on at most one slot, so outstanding retirements never exceed the
// total instance count), which means a push cannot fail in valid operation.
// The producer is the audio thread (a slot gives up an instance); the consumer
// is the logic thread (returns it to its pool in DrainRetire / at Set time).
class EffectRetireQueue {
public:
    explicit EffectRetireQueue(std::uint32_t capacityHint) {
        std::uint32_t cap = 1;
        // +1 so the ring can hold the full instance count without reading as
        // full one slot early.
        const std::uint32_t want = capacityHint + 1;
        while (cap < want) {
            cap <<= 1;
        }
        if (cap < 8) {
            cap = 8;
        }
        m_slots.resize(cap);
        m_mask = cap - 1;
    }

    // Producer (audio thread). Returns false only if the sizing invariant is
    // violated (cannot happen in valid operation); callers on the audio thread
    // ignore the result — there is nothing safe to do with a failure there,
    // and the sizing guarantees it never occurs.
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

// One pool per effect type plus the shared retire channel. Constructed once at
// Init; never moved afterwards (instance state holds pointers into this
// object's own storage).
namespace detail { struct EffectPoolTestAccess; }

class EffectPoolBank {
    enum class InstanceState : std::uint8_t {
        Free, Prepared, PreparedCommitHeld, Live, Exhausted
    };
    struct PoolTypeSlot;
public:
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
    };
    explicit EffectPoolBank(const EngineConfig& config);

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
    void DrainRetire();

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

    EffectRetireQueue& RetireQueue() { return m_retire; }

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
    // thin wrappers for the legacy scalars. An unknown type reports 0 (no slot).
    std::uint32_t CapacityOf(std::uint32_t type) const {
        const PoolTypeSlot* slot = SlotFor(type);
        return slot != nullptr ? slot->capacity : 0;
    }
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
    // buffer holds per-instance storage the registry row requests (the delay
    // line). Both vectors are sized once at Init and never resized, so a state
    // pointer computed by (type, index) is stable for the engine's life.
    struct PoolTypeSlot {
        struct CompressorRuntimeSlot {
            PinnedSnapshot3<CompressorRuntimePayload> published;
            CompressorRuntimePayload lastCoherent{};
        };
        std::uint32_t effectType = kEffectTypeNone;
        std::uint32_t strideUnits = 0; // std::max_align_t units per instance
        std::uint32_t capacity = 0;
        std::vector<std::max_align_t> storage;
        std::vector<float> extra;
        std::vector<std::uint32_t> freeList; // stack of free indices
        std::vector<std::uint64_t> generations;
        std::unique_ptr<std::atomic<InstanceState>[]> states;
        std::vector<EffectObjectIdentity> identities;
        std::vector<std::uint8_t> installed;
        std::unique_ptr<CompressorRuntimeSlot[]> compressorRuntime;
        std::uint32_t inUse = 0;
        std::uint32_t highWater = 0;
        std::uint32_t refusalCount = 0; // cumulative pool-exhaustion refusals

        void* StateAt(std::uint32_t index) {
            return &storage[static_cast<std::size_t>(index) * strideUnits];
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
    void Dispose(PoolTypeSlot& slot, const EffectInstanceRef& ref);

    // One slot per generated registry row, in registry (effect-type) order.
    std::vector<PoolTypeSlot> m_slots;

    EffectRetireQueue m_retire;
    // Audio-thread-owned cumulative totals. MixerLoop reads them only after
    // all per-host Compressor publications for the block have completed.
    std::uint64_t m_audioCompressorDetectorNonFiniteCount = 0;
    std::uint64_t m_audioCompressorDetectorSaturationCount = 0;
};

} // namespace harken

#endif // HARKEN_RENDER_EFFECT_POOL_H
