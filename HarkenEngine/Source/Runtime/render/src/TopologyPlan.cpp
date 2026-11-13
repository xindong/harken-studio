#include "harken/render/TopologyPlan.h"
#include "harken/render/SignalRenderPlan.h"

#include <algorithm>

namespace harken {

TopologyPlanPool::TopologyPlanPool(std::uint32_t capacity,
                                   std::uint32_t busCapacity,
                                   std::uint32_t slotsPerBus)
    : m_capacity(capacity),
      m_busCapacity(busCapacity),
      m_slotsPerBus(slotsPerBus),
      m_order(static_cast<std::size_t>(capacity) * busCapacity, 0),
      m_orderCount(capacity, 0),
      m_sends(capacity),
      m_outputLayouts(static_cast<std::size_t>(capacity) * busCapacity, 0),
      m_downmixCoeffs(static_cast<std::size_t>(capacity) * busCapacity * 64,
                      0.0f),
      m_parents(static_cast<std::size_t>(capacity) * busCapacity,
                kPlanParentUnset),
      m_alive(static_cast<std::size_t>(capacity) * busCapacity, 1),
      m_busIds(static_cast<std::size_t>(capacity) * busCapacity),
      m_generations(static_cast<std::size_t>(capacity) * busCapacity, 0),
      m_migrationTargets(static_cast<std::size_t>(capacity) * busCapacity, 0),
      m_preparedBusVolumeDb(static_cast<std::size_t>(capacity) * busCapacity,
                            0.0f),
      m_preparedBusOutputVolumeDb(
          static_cast<std::size_t>(capacity) * busCapacity, 0.0f),
      m_preparedBusVolumeRampMs(
          static_cast<std::size_t>(capacity) * busCapacity, 0),
      m_busVolumeApply(static_cast<std::size_t>(capacity) * busCapacity, 0),
      m_slotCount(capacity, 0),
      m_signals(capacity),
      m_effectUpdates(capacity),
      m_acquired(capacity, 0) {
    m_free.reserve(capacity);
    // Freelist stack: pop the lowest index first for deterministic slot reuse.
    for (std::uint32_t i = capacity; i-- > 0;) {
        m_free.push_back(i);
    }
    // Retire ring sized to hold the whole pool at once. +1 keeps a full ring
    // from reading as empty; rounded up to a power of two for mask indexing.
    std::uint32_t cap = 1;
    const std::uint32_t want = capacity + 1;
    while (cap < want) {
        cap <<= 1;
    }
    if (cap < 2) {
        cap = 2;
    }
    m_retire.resize(cap);
    m_retireMask = cap - 1;
}

std::uint32_t TopologyPlanPool::Acquire() {
    if (m_free.empty()) {
        return kInvalidTopologyPlanIndex;
    }
    const std::uint32_t index = m_free.back();
    std::vector<std::uint32_t> sendCounts(m_busCapacity, m_slotsPerBus);
    if (!PrepareSendRows(index, sendCounts)) return kInvalidTopologyPlanIndex;
    m_free.pop_back();
    m_acquired[index] = 1;
    ++m_inUse;
    if (m_inUse > m_highWater) {
        m_highWater = m_inUse;
    }
    // Clear the slot so a builder writes only what it populates.
    m_orderCount[index] = 0;
    // Clear the fold config to "no fold": a builder that ignores it publishes a
    // plan that folds no bus (layout 0, zero coefficients).
    const std::size_t fb = FoldBase(index);
    for (std::uint32_t i = 0; i < m_busCapacity; ++i) {
        m_outputLayouts[fb + i] = 0;
    }
    const std::size_t cb = CoeffBase(index);
    for (std::uint32_t i = 0; i < m_busCapacity * 64; ++i) {
        m_downmixCoeffs[cb + i] = 0.0f;
    }
    // Clear parents to leave-unchanged and liveness to alive: a builder that
    // ignores them publishes a plan that moves no tree edge and tombstones
    // nothing. slotCount 0 marks "no slot info" (adopted as orderCount).
    const std::size_t pb = FoldBase(index);
    for (std::uint32_t i = 0; i < m_busCapacity; ++i) {
        m_parents[pb + i] = kPlanParentUnset;
        m_alive[pb + i] = 1;
        m_busIds[pb + i] = {};
        m_generations[pb + i] = 0;
        m_migrationTargets[pb + i] = i;
        m_preparedBusVolumeDb[pb + i] = 0.0f;
        m_preparedBusOutputVolumeDb[pb + i] = 0.0f;
        m_preparedBusVolumeRampMs[pb + i] = 0;
        m_busVolumeApply[pb + i] = 0;
    }
    m_slotCount[index] = 0;
    m_effectUpdates[index].clear();
    return index;
}

bool TopologyPlanPool::CopyPlanFrom(const TopologyPlanPool& source,
                                    std::uint32_t sourceIndex,
                                    std::uint32_t destinationIndex) {
    if (sourceIndex >= source.m_capacity || destinationIndex >= m_capacity ||
        source.m_busCapacity != m_busCapacity ||
        source.m_slotsPerBus != m_slotsPerBus) {
        return false;
    }
    const std::size_t buses = m_busCapacity;
    const std::size_t coeffs = buses * 64;
    const std::size_t srcOrder = source.OrderBase(sourceIndex);
    const std::size_t dstOrder = OrderBase(destinationIndex);
    std::copy(source.m_order.begin() + srcOrder,
              source.m_order.begin() + srcOrder + buses,
              m_order.begin() + dstOrder);
    m_orderCount[destinationIndex] = source.m_orderCount[sourceIndex];
    try { m_sends[destinationIndex] = source.m_sends[sourceIndex]; }
    catch (...) { return false; }
    const std::size_t srcFold = source.FoldBase(sourceIndex);
    const std::size_t dstFold = FoldBase(destinationIndex);
    std::copy(source.m_outputLayouts.begin() + srcFold,
              source.m_outputLayouts.begin() + srcFold + buses,
              m_outputLayouts.begin() + dstFold);
    std::copy(source.m_parents.begin() + srcFold,
              source.m_parents.begin() + srcFold + buses,
              m_parents.begin() + dstFold);
    std::copy(source.m_alive.begin() + srcFold,
              source.m_alive.begin() + srcFold + buses,
              m_alive.begin() + dstFold);
    std::copy(source.m_busIds.begin() + srcFold,
              source.m_busIds.begin() + srcFold + buses,
              m_busIds.begin() + dstFold);
    std::copy(source.m_generations.begin() + srcFold,
              source.m_generations.begin() + srcFold + buses,
              m_generations.begin() + dstFold);
    std::copy(source.m_migrationTargets.begin() + srcFold,
              source.m_migrationTargets.begin() + srcFold + buses,
              m_migrationTargets.begin() + dstFold);
    std::copy(source.m_preparedBusVolumeDb.begin() + srcFold,
              source.m_preparedBusVolumeDb.begin() + srcFold + buses,
              m_preparedBusVolumeDb.begin() + dstFold);
    std::copy(source.m_preparedBusOutputVolumeDb.begin() + srcFold,
              source.m_preparedBusOutputVolumeDb.begin() + srcFold + buses,
              m_preparedBusOutputVolumeDb.begin() + dstFold);
    std::copy(source.m_preparedBusVolumeRampMs.begin() + srcFold,
              source.m_preparedBusVolumeRampMs.begin() + srcFold + buses,
              m_preparedBusVolumeRampMs.begin() + dstFold);
    std::copy(source.m_busVolumeApply.begin() + srcFold,
              source.m_busVolumeApply.begin() + srcFold + buses,
              m_busVolumeApply.begin() + dstFold);
    const std::size_t srcCoeff = source.CoeffBase(sourceIndex);
    const std::size_t dstCoeff = CoeffBase(destinationIndex);
    std::copy(source.m_downmixCoeffs.begin() + srcCoeff,
              source.m_downmixCoeffs.begin() + srcCoeff + coeffs,
              m_downmixCoeffs.begin() + dstCoeff);
    m_slotCount[destinationIndex] = source.m_slotCount[sourceIndex];
    m_signals[destinationIndex] = source.m_signals[sourceIndex];
    try {
        m_effectUpdates[destinationIndex] = source.m_effectUpdates[sourceIndex];
    } catch (...) {
        return false;
    }
    return true;
}

void TopologyPlanPool::Recycle(std::uint32_t planIndex) noexcept {
    if (!IsAcquired(planIndex) || m_free.size() >= m_free.capacity()) {
        return;
    }
    m_sends[planIndex] = SendStorage{};
    m_signals[planIndex].reset();
    m_effectUpdates[planIndex].clear();
    m_free.push_back(planIndex);
    m_acquired[planIndex] = 0;
    --m_inUse;
}

void TopologyPlanPool::SetEffectUpdates(std::uint32_t plan, std::vector<BusEffectChainAdoption> updates) {
    if (IsAcquired(plan)) m_effectUpdates[plan] = std::move(updates);
}

void TopologyPlanPool::SetSignals(std::uint32_t plan, std::shared_ptr<SignalRenderPlan> signals) {
    if (IsAcquired(plan)) m_signals[plan] = std::move(signals);
}

std::shared_ptr<SignalRenderPlan> TopologyPlanPool::Signals(std::uint32_t plan) const {
    return IsAcquired(plan) ? m_signals[plan] : nullptr;
}

void TopologyPlanPool::DrainRetire() {
    const std::uint64_t head = m_retireHead.load(std::memory_order_acquire);
    std::uint64_t tail = m_retireTail.load(std::memory_order_relaxed);
    while (tail != head) {
        Recycle(m_retire[tail & m_retireMask]);
        ++tail;
    }
    m_retireTail.store(tail, std::memory_order_release);
}

std::uint32_t* TopologyPlanPool::OrderData(std::uint32_t planIndex) {
    return m_order.data() + OrderBase(planIndex);
}

void TopologyPlanPool::SetOrderCount(std::uint32_t planIndex,
                                     std::uint32_t count) {
    if (planIndex < m_capacity) {
        m_orderCount[planIndex] = count;
    }
}

bool TopologyPlanPool::PrepareSendRows(std::uint32_t planIndex,
    std::span<const std::uint32_t> counts) {
    if (planIndex >= m_capacity || counts.size() > m_busCapacity) return false;
    try {
        SendStorage next;
        next.ranges.resize(m_busCapacity);
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] > UINT32_MAX - total) return false;
            next.ranges[i] = {static_cast<std::uint32_t>(total), counts[i]};
            total += counts[i];
        }
        next.slots.resize(static_cast<std::size_t>(total));
        next.targets.assign(static_cast<std::size_t>(total), kEmptySendTarget);
        next.levels.resize(static_cast<std::size_t>(total));
        next.apply.resize(static_cast<std::size_t>(total));
        next.ramps.resize(static_cast<std::size_t>(total));
        for (const auto range : next.ranges)
            for (std::uint32_t slot = 0; slot < range.count; ++slot)
                next.slots[range.first + slot] = slot;
        m_sends[planIndex] = std::move(next);
        return true;
    } catch (...) { return false; }
}

std::uint32_t* TopologyPlanPool::SendSlotsRow(std::uint32_t planIndex,
    std::uint32_t fromBus) {
    auto& sends = m_sends[planIndex];
    return sends.slots.empty() ? nullptr
        : sends.slots.data() + sends.ranges[fromBus].first;
}

std::uint32_t* TopologyPlanPool::SendTargetsRow(std::uint32_t planIndex,
    std::uint32_t fromBus) {
    auto& sends = m_sends[planIndex];
    return sends.targets.empty() ? nullptr
        : sends.targets.data() + sends.ranges[fromBus].first;
}

float* TopologyPlanPool::PreparedSendLevelsRow(std::uint32_t planIndex,
    std::uint32_t fromBus) {
    auto& sends = m_sends[planIndex];
    return sends.levels.empty() ? nullptr
        : sends.levels.data() + sends.ranges[fromBus].first;
}

std::uint8_t* TopologyPlanPool::SendLevelApplyRow(std::uint32_t planIndex,
    std::uint32_t fromBus) {
    auto& sends = m_sends[planIndex];
    return sends.apply.empty() ? nullptr
        : sends.apply.data() + sends.ranges[fromBus].first;
}

std::uint32_t* TopologyPlanPool::SendLevelRampMsRow(std::uint32_t planIndex,
    std::uint32_t fromBus) {
    auto& sends = m_sends[planIndex];
    return sends.ramps.empty() ? nullptr
        : sends.ramps.data() + sends.ranges[fromBus].first;
}

std::uint32_t* TopologyPlanPool::OutputLayoutData(std::uint32_t planIndex) {
    return m_outputLayouts.data() + FoldBase(planIndex);
}

float* TopologyPlanPool::DownmixCoeffsData(std::uint32_t planIndex) {
    return m_downmixCoeffs.data() + CoeffBase(planIndex);
}

std::uint32_t* TopologyPlanPool::ParentsData(std::uint32_t planIndex) {
    return m_parents.data() + FoldBase(planIndex);
}

std::uint8_t* TopologyPlanPool::AliveData(std::uint32_t planIndex) {
    return m_alive.data() + FoldBase(planIndex);
}

VnBusId* TopologyPlanPool::BusIdsData(std::uint32_t planIndex) {
    return m_busIds.data() + FoldBase(planIndex);
}

std::uint32_t* TopologyPlanPool::GenerationsData(
    std::uint32_t planIndex) {
    return m_generations.data() + FoldBase(planIndex);
}

std::uint32_t* TopologyPlanPool::MigrationTargetsData(
    std::uint32_t planIndex) {
    return m_migrationTargets.data() + FoldBase(planIndex);
}

float* TopologyPlanPool::PreparedBusVolumeDbData(
    std::uint32_t planIndex) {
    return m_preparedBusVolumeDb.data() + FoldBase(planIndex);
}

float* TopologyPlanPool::PreparedBusOutputVolumeDbData(
    std::uint32_t planIndex) {
    return m_preparedBusOutputVolumeDb.data() + FoldBase(planIndex);
}

std::uint32_t* TopologyPlanPool::PreparedBusVolumeRampMsData(
    std::uint32_t planIndex) {
    return m_preparedBusVolumeRampMs.data() + FoldBase(planIndex);
}

std::uint8_t* TopologyPlanPool::BusVolumeApplyData(
    std::uint32_t planIndex) {
    return m_busVolumeApply.data() + FoldBase(planIndex);
}

void TopologyPlanPool::SetSlotCount(std::uint32_t planIndex,
                                    std::uint32_t count) {
    if (planIndex < m_capacity) {
        m_slotCount[planIndex] = count;
    }
}

TopologyPlanView TopologyPlanPool::View(std::uint32_t planIndex) const {
    TopologyPlanView v;
    if (!IsAcquired(planIndex)) {
        return v; // valid == false
    }
    v.order = m_order.data() + OrderBase(planIndex);
    v.orderCount = m_orderCount[planIndex];
    const auto& sends = m_sends[planIndex];
    v.sendRanges = sends.ranges.data();
    v.sendSlots = sends.slots.data();
    v.sendTargets = sends.targets.data();
    v.preparedSendLevels = sends.levels.data();
    v.sendLevelApply = sends.apply.data();
    v.sendLevelRampMs = sends.ramps.data();
    v.sendCount = static_cast<std::uint32_t>(sends.targets.size());
    v.slotsPerBus = m_slotsPerBus;
    v.outputLayouts = m_outputLayouts.data() + FoldBase(planIndex);
    v.downmixCoeffs = m_downmixCoeffs.data() + CoeffBase(planIndex);
    v.parents = m_parents.data() + FoldBase(planIndex);
    v.alive = m_alive.data() + FoldBase(planIndex);
    v.busIds = m_busIds.data() + FoldBase(planIndex);
    v.generations = m_generations.data() + FoldBase(planIndex);
    v.migrationTargets =
        m_migrationTargets.data() + FoldBase(planIndex);
    v.preparedBusVolumeDb =
        m_preparedBusVolumeDb.data() + FoldBase(planIndex);
    v.preparedBusOutputVolumeDb =
        m_preparedBusOutputVolumeDb.data() + FoldBase(planIndex);
    v.preparedBusVolumeRampMs =
        m_preparedBusVolumeRampMs.data() + FoldBase(planIndex);
    v.busVolumeApply = m_busVolumeApply.data() + FoldBase(planIndex);
    v.slotCount = m_slotCount[planIndex];
    v.signals = m_signals[planIndex].get();
    v.effectUpdates = m_effectUpdates[planIndex].data();
    v.effectUpdateCount = static_cast<std::uint32_t>(m_effectUpdates[planIndex].size());
    v.valid = true;
    return v;
}

void TopologyPlanPool::Retire(std::uint32_t planIndex) {
    const std::uint64_t head = m_retireHead.load(std::memory_order_relaxed);
    const std::uint64_t tail = m_retireTail.load(std::memory_order_acquire);
    if (head - tail >= m_retire.size()) {
        return; // cannot happen in valid operation (sized to hold the pool)
    }
    m_retire[head & m_retireMask] = planIndex;
    m_retireHead.store(head + 1, std::memory_order_release);
}

} // namespace harken
