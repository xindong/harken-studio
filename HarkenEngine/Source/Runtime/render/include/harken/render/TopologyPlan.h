// Bus-topology plan pool and the audio->logic retirement channel realizing the
// structural half of the unified bus model. A topology plan is STRUCTURE ONLY:
// it carries the feeds-first bus evaluation order and the bus->bus send-edge
// membership (per-bus target lists). Send levels, level ramps, and per-bus
// state (fader point, effect slots, bypass) are NOT plan state — they are
// ordinary audio-thread state driven by their own commands, so changing a level
// republishes nothing.
//
// Ownership mirrors the effect-instance pattern exactly: the logic thread
// constructs a plan into a free pool slot, hands the slot index to the audio
// thread by command, and never touches it again. The audio thread consumes the
// plan and NEVER frees it; a superseded plan is named on the retire queue by
// the audio thread and returned to the freelist by the logic thread in
// DrainRetire. All storage is preallocated at Init (a flat vector indexed by
// plan slot), so publishing a plan and installing one both touch no heap.
#ifndef HARKEN_RENDER_TOPOLOGY_PLAN_H
#define HARKEN_RENDER_TOPOLOGY_PLAN_H

#include "harken/core/EngineConfig.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace harken {

inline constexpr std::uint32_t kInvalidTopologyPlanIndex = 0xFFFFFFFFu;
inline constexpr std::uint8_t kApplyBusVolume = 1;
inline constexpr std::uint8_t kApplyBusOutputVolume = 2;
inline constexpr std::uint8_t kApplyBusNeutralProperties = 4;

// Empty send-target sentinel inside a plan's membership row. Numerically equal
// to kInvalidBusIndex (BusTree.h); duplicated here so this header stays free of
// the bus-tree dependency (BusTree.h includes this one).
inline constexpr std::uint32_t kEmptySendTarget = 0xFFFFFFFFu;

// Unset entry in a plan's parents array (same numeric sentinel, same
// duplication rationale): the swap leaves that bus's audio-side parent
// untouched, so a plan built without parent info (e.g. directly by a test)
// changes no tree edge.
inline constexpr std::uint32_t kPlanParentUnset = 0xFFFFFFFFu;

// Read-only view of one published plan, resolved by index from the pool. Cheap
// to copy; both threads use it (the audio thread walks it every block, the logic
// thread reads membership to reconcile). Non-owning: valid while the plan slot
// stays live.
struct TopologyPlanView {
    const std::uint32_t* order = nullptr; // orderCount dense bus indices,
                                          // feeds-first (every bus after all
                                          // buses that feed it; master last)
    std::uint32_t orderCount = 0;
    const std::uint32_t* sendTargets = nullptr; // busCapacity rows of
                                                 // slotsPerBus entries, row-major
                                                 // by from-bus. Slot identity is
                                                 // authoritative and stable;
                                                 // kEmptySendTarget marks unused.
    // Optional per-membership-slot level action, indexed exactly like
    // sendTargets. A set flag snaps that exact audio edge slot to the prepared
    // logic-side linear gain and clears its ramp bookkeeping; a clear flag
    // preserves the live level/ramp only when slot and target both match.
    const float* preparedSendLevels = nullptr;
    const std::uint8_t* sendLevelApply = nullptr;
    const std::uint32_t* sendLevelRampMs = nullptr;
    std::uint32_t slotsPerBus = 0;
    // Per-bus output fold config (the multichannel amendment): outputLayouts is
    // busCapacity VnChannelLayout values (each bus's presented output layout;
    // the master's is the device layout, a value equal to the mix layout means
    // no fold), and downmixCoeffs is busCapacity dst x src coefficient tables of
    // 64 floats each, row-major with the destination the outer index. Carried
    // in the plan so Vn_SetBusOutputLayout / Vn_SetBusDownmixMatrix reach the
    // audio thread through the existing plan-swap command (zero new opcode).
    const std::uint32_t* outputLayouts = nullptr;
    const float* downmixCoeffs = nullptr;
    // Per-slot tree parents and liveness (carried like the fold config, so a
    // reparent or destroy reaches the audio thread through the existing
    // plan-swap command with zero new opcodes). parents[i] is dense bus i's
    // parent index, or kPlanParentUnset to leave the audio-side parent
    // untouched. alive[i] is the slot's liveness (0 = tombstoned). slotCount
    // is the dense-index high water the plan was built against (the logic
    // BusCount at publish); 0 marks a plan with no slot info, adopted as
    // orderCount (the pre-amendment meaning where the two coincide).
    const std::uint32_t* parents = nullptr;
    const std::uint8_t* alive = nullptr;
    const VnBusId* busIds = nullptr;
    const std::uint32_t* generations = nullptr;
    const std::uint32_t* migrationTargets = nullptr;
    const float* preparedBusVolumeDb = nullptr;
    const float* preparedBusOutputVolumeDb = nullptr;
    const std::uint32_t* preparedBusVolumeRampMs = nullptr;
    const std::uint8_t* busVolumeApply = nullptr;
    std::uint32_t slotCount = 0;
    bool valid = false;
};

class TopologyPlanPool {
public:
    // capacity plans, each sized for busCapacity buses and slotsPerBus send
    // edges per bus. All storage is allocated here and never resized.
    TopologyPlanPool(std::uint32_t capacity, std::uint32_t busCapacity,
                     std::uint32_t slotsPerBus);

    TopologyPlanPool(const TopologyPlanPool&) = delete;
    TopologyPlanPool& operator=(const TopologyPlanPool&) = delete;

    // --- logic thread ---

    // Takes a free plan slot to build into, or kInvalidTopologyPlanIndex when
    // the pool is exhausted. The slot's contents are cleared (order empty, every
    // send target kEmptySendTarget) so a builder only writes what it populates.
    std::uint32_t Acquire();

    // Returns a retired plan slot to the freelist. Bounds-checked; ignores
    // kInvalidTopologyPlanIndex.
    void Recycle(std::uint32_t planIndex) noexcept;
    bool IsAcquired(std::uint32_t planIndex) const noexcept {
        return planIndex < m_capacity && m_acquired[planIndex] != 0;
    }

    // Drains every pending retirement into the freelist. Called at each
    // structural change (to free the slot a prior swap superseded) and each
    // Vn_Update.
    void DrainRetire();

    // Build helpers, used on an acquired slot before it is published. The
    // pointers address stable pool storage for the plan's life.
    std::uint32_t* OrderData(std::uint32_t planIndex);
    void SetOrderCount(std::uint32_t planIndex, std::uint32_t count);
    std::uint32_t* SendTargetsRow(std::uint32_t planIndex,
                                  std::uint32_t fromBus);
    float* PreparedSendLevelsRow(std::uint32_t planIndex,
                                 std::uint32_t fromBus);
    std::uint8_t* SendLevelApplyRow(std::uint32_t planIndex,
                                    std::uint32_t fromBus);
    std::uint32_t* SendLevelRampMsRow(std::uint32_t planIndex,
                                     std::uint32_t fromBus);
    // Per-bus output fold config storage for a plan: busCapacity output layouts
    // and busCapacity 64-float coefficient tables. The builder copies the
    // logic-side authoritative fold config here before publishing; Acquire()
    // clears both to "no fold" so a builder that ignores them publishes a plan
    // that leaves every bus unfolded.
    std::uint32_t* OutputLayoutData(std::uint32_t planIndex);
    float* DownmixCoeffsData(std::uint32_t planIndex);
    // Tree-parent and liveness storage for a plan: busCapacity entries each.
    // Acquire() clears parents to kPlanParentUnset (leave-unchanged) and
    // liveness to alive, so a builder that ignores them publishes a plan that
    // moves no tree edge and tombstones nothing.
    std::uint32_t* ParentsData(std::uint32_t planIndex);
    std::uint8_t* AliveData(std::uint32_t planIndex);
    VnBusId* BusIdsData(std::uint32_t planIndex);
    std::uint32_t* GenerationsData(std::uint32_t planIndex);
    std::uint32_t* MigrationTargetsData(std::uint32_t planIndex);
    float* PreparedBusVolumeDbData(std::uint32_t planIndex);
    float* PreparedBusOutputVolumeDbData(std::uint32_t planIndex);
    std::uint32_t* PreparedBusVolumeRampMsData(std::uint32_t planIndex);
    std::uint8_t* BusVolumeApplyData(std::uint32_t planIndex);
    void SetSlotCount(std::uint32_t planIndex, std::uint32_t count);
    bool CopyPlanFrom(const TopologyPlanPool& source, std::uint32_t sourceIndex,
                      std::uint32_t destinationIndex);

    // --- both threads (read-only, stable storage) ---

    TopologyPlanView View(std::uint32_t planIndex) const;

    std::uint32_t SlotsPerBus() const { return m_slotsPerBus; }
    std::uint32_t BusCapacity() const { return m_busCapacity; }

    // --- audio thread ---

    // Names a superseded plan for the logic thread to reclaim. Non-lossy: a plan
    // is installed at most once, so outstanding retirements never exceed the
    // pool capacity and the push cannot fail in valid operation.
    void Retire(std::uint32_t planIndex);

    // --- observability (logic thread) ---

    std::uint32_t Capacity() const { return m_capacity; }
    std::uint32_t InUse() const { return m_inUse; }
    std::uint32_t HighWater() const { return m_highWater; }

private:
    std::size_t OrderBase(std::uint32_t planIndex) const {
        return static_cast<std::size_t>(planIndex) * m_busCapacity;
    }
    std::size_t EdgeBase(std::uint32_t planIndex) const {
        return static_cast<std::size_t>(planIndex) * m_busCapacity *
               m_slotsPerBus;
    }
    std::size_t FoldBase(std::uint32_t planIndex) const {
        return static_cast<std::size_t>(planIndex) * m_busCapacity;
    }
    std::size_t CoeffBase(std::uint32_t planIndex) const {
        return static_cast<std::size_t>(planIndex) * m_busCapacity * 64;
    }

    std::uint32_t m_capacity;
    std::uint32_t m_busCapacity;
    std::uint32_t m_slotsPerBus;

    // Flat, index-addressed storage sized once at construction.
    std::vector<std::uint32_t> m_order;       // capacity * busCapacity
    std::vector<std::uint32_t> m_orderCount;  // capacity
    std::vector<std::uint32_t> m_sendTargets; // capacity * busCapacity * slots
    std::vector<float> m_preparedSendLevels;  // same shape as m_sendTargets
    std::vector<std::uint8_t> m_sendLevelApply; // same shape, 0 = preserve
    std::vector<std::uint32_t> m_sendLevelRampMs; // same shape
    std::vector<std::uint32_t> m_outputLayouts; // capacity * busCapacity
    std::vector<float> m_downmixCoeffs;         // capacity * busCapacity * 64
    std::vector<std::uint32_t> m_parents;       // capacity * busCapacity
    std::vector<std::uint8_t> m_alive;          // capacity * busCapacity
    std::vector<VnBusId> m_busIds;         // capacity * busCapacity
    std::vector<std::uint32_t> m_generations;    // capacity * busCapacity
    std::vector<std::uint32_t> m_migrationTargets; // capacity * busCapacity
    std::vector<float> m_preparedBusVolumeDb;      // capacity * busCapacity
    std::vector<float> m_preparedBusOutputVolumeDb;
    std::vector<std::uint32_t> m_preparedBusVolumeRampMs;
    std::vector<std::uint8_t> m_busVolumeApply;
    std::vector<std::uint32_t> m_slotCount;     // capacity

    std::vector<std::uint8_t> m_acquired;
    std::vector<std::uint32_t> m_free; // freelist stack of plan indices
    std::uint32_t m_inUse = 0;
    std::uint32_t m_highWater = 0;

    // Audio->logic SPSC ring of retired plan indices. Sized to hold the whole
    // pool at once (+1 so full is never read one slot early), so a push cannot
    // fail in valid operation.
    std::vector<std::uint32_t> m_retire;
    std::uint32_t m_retireMask = 0;
    std::atomic<std::uint64_t> m_retireHead{0}; // producer-owned (audio)
    std::atomic<std::uint64_t> m_retireTail{0}; // consumer-owned (logic)
};

class ProjectGenerationInstallAckRingV1 {
public:
    explicit ProjectGenerationInstallAckRingV1(std::uint32_t planCapacity)
        : m_slots(RingCapacity(planCapacity), ProjectGenerationInstallAckV1{}),
          m_mask(m_slots.empty()
                     ? 0u
                     : static_cast<std::uint32_t>(m_slots.size() - 1u)) {}

    void Push(const ProjectGenerationInstallAckV1& record) noexcept {
        if (m_slots.empty()) {
            return;
        }
        const std::uint64_t head = m_head.load(std::memory_order_relaxed);
        const std::uint64_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= m_slots.size()) {
            return;
        }
        m_slots[static_cast<std::size_t>(head & m_mask)] = record;
        m_head.store(head + 1u, std::memory_order_release);
    }

    std::uint32_t Drain(ProjectGenerationInstallAckV1* out,
                        std::uint32_t maxCount) noexcept {
        const std::uint64_t head = m_head.load(std::memory_order_acquire);
        std::uint64_t tail = m_tail.load(std::memory_order_relaxed);
        std::uint32_t count = 0;
        while (tail != head && count < maxCount) {
            if (out != nullptr) {
                out[count] = m_slots[static_cast<std::size_t>(tail & m_mask)];
            }
            ++tail;
            ++count;
        }
        m_tail.store(tail, std::memory_order_release);
        return count;
    }

    std::uint32_t Capacity() const noexcept {
        return static_cast<std::uint32_t>(m_slots.size());
    }

    static std::uint32_t RingCapacity(std::uint32_t planCapacity) noexcept {
        std::uint32_t cap = 1;
        const std::uint32_t want = planCapacity + 1u;
        while (cap < want) {
            cap <<= 1;
        }
        if (cap < 2u) {
            cap = 2u;
        }
        return cap;
    }

private:
    std::vector<ProjectGenerationInstallAckV1> m_slots;
    std::uint32_t m_mask = 0;
    std::atomic<std::uint64_t> m_head{0};
    std::atomic<std::uint64_t> m_tail{0};
};

} // namespace harken

#endif // HARKEN_RENDER_TOPOLOGY_PLAN_H
