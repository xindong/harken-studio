#include "harken/render/TopologyPlan.h"

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
      m_sendTargets(static_cast<std::size_t>(capacity) * busCapacity *
                        slotsPerBus,
                    kEmptySendTarget),
      m_preparedSendLevels(static_cast<std::size_t>(capacity) * busCapacity *
                               slotsPerBus,
                           0.0f),
      m_sendLevelApply(static_cast<std::size_t>(capacity) * busCapacity *
                           slotsPerBus,
                       0),
      m_sendLevelRampMs(static_cast<std::size_t>(capacity) * busCapacity *
                            slotsPerBus,
                        0),
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
    m_free.pop_back();
    m_acquired[index] = 1;
    ++m_inUse;
    if (m_inUse > m_highWater) {
        m_highWater = m_inUse;
    }
    // Clear the slot so a builder writes only what it populates.
    m_orderCount[index] = 0;
    const std::size_t eb = EdgeBase(index);
    for (std::uint32_t i = 0; i < m_busCapacity * m_slotsPerBus; ++i) {
        m_sendTargets[eb + i] = kEmptySendTarget;
        m_preparedSendLevels[eb + i] = 0.0f;
        m_sendLevelApply[eb + i] = 0;
        m_sendLevelRampMs[eb + i] = 0;
    }
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
    const std::size_t edges = static_cast<std::size_t>(m_busCapacity) * m_slotsPerBus;
    const std::size_t coeffs = buses * 64;
    const std::size_t srcOrder = source.OrderBase(sourceIndex);
    const std::size_t dstOrder = OrderBase(destinationIndex);
    std::copy(source.m_order.begin() + srcOrder,
              source.m_order.begin() + srcOrder + buses,
              m_order.begin() + dstOrder);
    m_orderCount[destinationIndex] = source.m_orderCount[sourceIndex];
    const std::size_t srcEdge = source.EdgeBase(sourceIndex);
    const std::size_t dstEdge = EdgeBase(destinationIndex);
    std::copy(source.m_sendTargets.begin() + srcEdge,
              source.m_sendTargets.begin() + srcEdge + edges,
              m_sendTargets.begin() + dstEdge);
    std::copy(source.m_preparedSendLevels.begin() + srcEdge,
              source.m_preparedSendLevels.begin() + srcEdge + edges,
              m_preparedSendLevels.begin() + dstEdge);
    std::copy(source.m_sendLevelApply.begin() + srcEdge,
              source.m_sendLevelApply.begin() + srcEdge + edges,
              m_sendLevelApply.begin() + dstEdge);
    std::copy(source.m_sendLevelRampMs.begin() + srcEdge,
              source.m_sendLevelRampMs.begin() + srcEdge + edges,
              m_sendLevelRampMs.begin() + dstEdge);
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
    return true;
}

void TopologyPlanPool::Recycle(std::uint32_t planIndex) noexcept {
    if (!IsAcquired(planIndex) || m_free.size() >= m_free.capacity()) {
        return;
    }
    m_free.push_back(planIndex);
    m_acquired[planIndex] = 0;
    --m_inUse;
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

std::uint32_t* TopologyPlanPool::SendTargetsRow(std::uint32_t planIndex,
                                                std::uint32_t fromBus) {
    return m_sendTargets.data() + EdgeBase(planIndex) +
           static_cast<std::size_t>(fromBus) * m_slotsPerBus;
}

float* TopologyPlanPool::PreparedSendLevelsRow(std::uint32_t planIndex,
                                               std::uint32_t fromBus) {
    return m_preparedSendLevels.data() + EdgeBase(planIndex) +
           static_cast<std::size_t>(fromBus) * m_slotsPerBus;
}

std::uint8_t* TopologyPlanPool::SendLevelApplyRow(
    std::uint32_t planIndex, std::uint32_t fromBus) {
    return m_sendLevelApply.data() + EdgeBase(planIndex) +
           static_cast<std::size_t>(fromBus) * m_slotsPerBus;
}

std::uint32_t* TopologyPlanPool::SendLevelRampMsRow(
    std::uint32_t planIndex, std::uint32_t fromBus) {
    return m_sendLevelRampMs.data() + EdgeBase(planIndex) +
           static_cast<std::size_t>(fromBus) * m_slotsPerBus;
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
    if (planIndex >= m_capacity) {
        return v; // valid == false
    }
    v.order = m_order.data() + OrderBase(planIndex);
    v.orderCount = m_orderCount[planIndex];
    v.sendTargets = m_sendTargets.data() + EdgeBase(planIndex);
    v.preparedSendLevels =
        m_preparedSendLevels.data() + EdgeBase(planIndex);
    v.sendLevelApply = m_sendLevelApply.data() + EdgeBase(planIndex);
    v.sendLevelRampMs = m_sendLevelRampMs.data() + EdgeBase(planIndex);
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
