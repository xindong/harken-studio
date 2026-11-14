#include "harken/render/TopologyPlan.h"
#include "harken/core/ChannelLayout.h"
#include "harken/render/SignalRenderPlan.h"
#include "harken/core/LogicAllocCheckpoint.h"

#include <algorithm>

namespace harken {

TopologyPlanPool::TopologyPlanPool(std::uint32_t capacity,
                                   std::uint32_t busCapacity,
                                   std::uint32_t slotsPerBus)
    : m_capacity(capacity),
      m_busCapacity(busCapacity),
      m_slotsPerBus(slotsPerBus),
      m_orderCount(capacity, 0),
      m_storage(capacity),
      m_slotCount(capacity, 0),
      m_signals(capacity),
      m_effectUpdates(capacity),
      m_acquired(capacity, 0) {
    m_buildScratch.feeders.resize(busCapacity);
    m_buildScratch.emittedFeeders.resize(busCapacity);
    m_buildScratch.emitted.resize(busCapacity);
    m_buildScratch.sendCounts.resize(busCapacity);
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
    return AcquireImpl(true, 0);
}

std::uint32_t TopologyPlanPool::AcquireSparse(std::uint32_t sendAllowance) {
    return AcquireImpl(false, sendAllowance);
}

std::uint32_t TopologyPlanPool::AcquireImpl(bool legacyRows, std::uint32_t sendAllowance) {
    if (m_free.empty()) {
        return kInvalidTopologyPlanIndex;
    }
    const std::uint32_t index = m_free.back();
    auto& storage = m_storage[index];
    try {
        storage.order.assign(m_busCapacity, 0);
        storage.outputLayouts.assign(m_busCapacity, 0);
        storage.parents.assign(m_busCapacity, kPlanParentUnset);
        storage.alive.assign(m_busCapacity, 1);
        storage.busIds.resize(m_busCapacity);
        storage.busGenerations.assign(m_busCapacity, 0);
        storage.migrationTargets.resize(m_busCapacity);
        for (std::uint32_t i = 0; i < m_busCapacity; ++i) storage.migrationTargets[i] = i;
        storage.preparedBusVolumeDb.assign(m_busCapacity, 0);
        storage.preparedBusOutputVolumeDb.assign(m_busCapacity, 0);
        storage.preparedBusVolumeRampMs.assign(m_busCapacity, 0);
        storage.busVolumeApply.assign(m_busCapacity, 0);
        auto& counts = m_buildScratch.sendCounts;
        std::fill(counts.begin(), counts.end(), legacyRows ? m_slotsPerBus : 0);
        if (!PrepareSendRows(index, counts) || !ReserveSendCapacity(index, sendAllowance)) {
            storage = {};
            return kInvalidTopologyPlanIndex;
        }
    } catch (...) {
        storage = {};
        return kInvalidTopologyPlanIndex;
    }
    m_free.pop_back();
    m_acquired[index] = 1;
    m_highWater = std::max(m_highWater, ++m_inUse);
    m_orderCount[index] = m_slotCount[index] = 0;
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
    if (!source.IsAcquired(sourceIndex) || !IsAcquired(destinationIndex)) return false;
    try {
        const auto& prior = source.m_storage[sourceIndex];
        PlanStorage next;
        next.order = prior.order;
        next.outputLayouts = prior.outputLayouts;
        next.customDownmix = prior.customDownmix;
        next.mixLayout = prior.mixLayout;
        next.parents = prior.parents;
        next.alive = prior.alive;
        next.busIds = prior.busIds;
        next.busGenerations = prior.busGenerations;
        next.generations = prior.generations;
        next.migrationTargets = prior.migrationTargets;
        next.preparedBusVolumeDb = prior.preparedBusVolumeDb;
        next.preparedBusOutputVolumeDb = prior.preparedBusOutputVolumeDb;
        next.preparedBusVolumeRampMs = prior.preparedBusVolumeRampMs;
        next.busVolumeApply = prior.busVolumeApply;
        next.ranges = prior.ranges;
        next.slots = prior.slots;
        next.targets = prior.targets;
        next.levels = prior.levels;
        next.apply = prior.apply;
        next.ramps = prior.ramps;
        // Send envelopes belong to audio; continuity adopts them at publication.
        next.states.resize(prior.states.size());
        auto effects = source.m_effectUpdates[sourceIndex];
        m_storage[destinationIndex] = std::move(next);
        m_effectUpdates[destinationIndex] = std::move(effects);
        m_orderCount[destinationIndex] = source.m_orderCount[sourceIndex];
        m_slotCount[destinationIndex] = source.m_slotCount[sourceIndex];
        m_signals[destinationIndex] = source.m_signals[sourceIndex];
        return true;
    } catch (...) { return false; }
}

void TopologyPlanPool::Recycle(std::uint32_t planIndex) noexcept {
    if (!IsAcquired(planIndex) || m_free.size() >= m_free.capacity()) {
        return;
    }
    m_storage[planIndex] = PlanStorage{};
    m_signals[planIndex].reset();
    std::vector<BusEffectChainAdoption>{}.swap(m_effectUpdates[planIndex]);
    m_free.push_back(planIndex);
    m_acquired[planIndex] = 0;
    --m_inUse;
}

void TopologyPlanPool::SetEffectUpdates(std::uint32_t plan, std::vector<BusEffectChainAdoption> updates) {
    if (IsAcquired(plan)) m_effectUpdates[plan] = std::move(updates);
}

std::size_t TopologyPlanPool::StorageBytes() const noexcept {
    const auto bytes = [](const auto& rows) { return rows.capacity() * sizeof(rows[0]); };
    std::size_t total = sizeof(*this) + bytes(m_storage) + bytes(m_orderCount) + bytes(m_slotCount) +
        bytes(m_signals) + bytes(m_effectUpdates) + bytes(m_acquired) + bytes(m_free) + bytes(m_retire) +
        bytes(m_buildScratch.feeders) + bytes(m_buildScratch.emittedFeeders) +
        bytes(m_buildScratch.emitted) + bytes(m_buildScratch.sendCounts);
    for (const auto& row : m_storage)
        total += bytes(row.order) + bytes(row.outputLayouts) + bytes(row.parents) + bytes(row.busGenerations) +
            bytes(row.migrationTargets) + bytes(row.customDownmix) + bytes(row.preparedBusVolumeDb) +
            bytes(row.preparedBusOutputVolumeDb) + bytes(row.preparedBusVolumeRampMs) + bytes(row.alive) +
            bytes(row.busVolumeApply) + bytes(row.busIds) + bytes(row.ranges) + bytes(row.slots) +
            bytes(row.generations) + bytes(row.targets) + bytes(row.levels) + bytes(row.apply) +
            bytes(row.ramps) + bytes(row.states);
    for (const auto& updates : m_effectUpdates) total += bytes(updates);
    return total;
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
    return m_storage[planIndex].order.data();
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
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] > UINT32_MAX - total) return false;
            total += counts[i];
        }
        auto& next = m_storage[planIndex];
        // Reserve every possibly failing allocation before changing contents.
        // Acquire prepares the direct-mutation allowance; FillPlan then only
        // rewrites unpublished rows within that allowance.
        if (!ReserveSendCapacity(planIndex, static_cast<std::uint32_t>(total))) return false;
        next.ranges.assign(m_busCapacity, {});
        std::uint32_t first = 0;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            next.ranges[i] = {first, counts[i]};
            first += counts[i];
        }
        next.slots.resize(static_cast<std::size_t>(total));
        next.generations.assign(static_cast<std::size_t>(total), 0);
        next.targets.assign(static_cast<std::size_t>(total), kEmptySendTarget);
        next.levels.assign(static_cast<std::size_t>(total), 0.0f);
        next.apply.assign(static_cast<std::size_t>(total), 0);
        next.ramps.assign(static_cast<std::size_t>(total), 0);
        next.states.assign(static_cast<std::size_t>(total), {});
        for (const auto range : next.ranges)
            for (std::uint32_t slot = 0; slot < range.count; ++slot)
                next.slots[range.first + slot] = slot;
        return true;
    } catch (...) { return false; }
}

bool TopologyPlanPool::ReserveSendCapacity(std::uint32_t planIndex, std::uint32_t count) {
    if (planIndex >= m_capacity) return false;
    try {
        auto& next = m_storage[planIndex];
        const auto reserve = [](auto& values, std::size_t size) {
            detail::CheckedGrow(values, size, detail::LogicAllocSite::BusSendStorage);
            values.reserve(size);
        };
        reserve(next.ranges, m_busCapacity);
        reserve(next.slots, count);
        reserve(next.generations, count);
        reserve(next.targets, count);
        reserve(next.levels, count);
        reserve(next.apply, count);
        reserve(next.ramps, count);
        reserve(next.states, count);
        return true;
    } catch (...) { return false; }
}

std::uint32_t* TopologyPlanPool::SendSlotsRow(std::uint32_t planIndex,
    std::uint32_t fromBus) {
    auto& sends = m_storage[planIndex];
    return sends.slots.empty() ? nullptr
        : sends.slots.data() + sends.ranges[fromBus].first;
}

std::uint64_t* TopologyPlanPool::SendGenerationsRow(std::uint32_t planIndex,
    std::uint32_t fromBus) {
    auto& sends = m_storage[planIndex];
    return sends.generations.empty() ? nullptr
        : sends.generations.data() + sends.ranges[fromBus].first;
}

std::uint32_t* TopologyPlanPool::SendTargetsRow(std::uint32_t planIndex,
    std::uint32_t fromBus) {
    auto& sends = m_storage[planIndex];
    return sends.targets.empty() ? nullptr
        : sends.targets.data() + sends.ranges[fromBus].first;
}

float* TopologyPlanPool::PreparedSendLevelsRow(std::uint32_t planIndex,
    std::uint32_t fromBus) {
    auto& sends = m_storage[planIndex];
    return sends.levels.empty() ? nullptr
        : sends.levels.data() + sends.ranges[fromBus].first;
}

std::uint8_t* TopologyPlanPool::SendLevelApplyRow(std::uint32_t planIndex,
    std::uint32_t fromBus) {
    auto& sends = m_storage[planIndex];
    return sends.apply.empty() ? nullptr
        : sends.apply.data() + sends.ranges[fromBus].first;
}

std::uint32_t* TopologyPlanPool::SendLevelRampMsRow(std::uint32_t planIndex,
    std::uint32_t fromBus) {
    auto& sends = m_storage[planIndex];
    return sends.ramps.empty() ? nullptr
        : sends.ramps.data() + sends.ranges[fromBus].first;
}

std::uint32_t* TopologyPlanPool::OutputLayoutData(std::uint32_t planIndex) {
    return m_storage[planIndex].outputLayouts.data();
}

void TopologyPlanPool::SetMixLayout(std::uint32_t planIndex, std::uint32_t layout) noexcept {
    m_storage[planIndex].mixLayout = layout;
}

void TopologyPlanPool::SetCustomDownmix(std::uint32_t planIndex, std::uint32_t bus,
    BusCustomDownmix matrix) {
    auto& rows = m_storage[planIndex].customDownmix;
    const auto it = std::lower_bound(rows.begin(), rows.end(), bus,
        [](const auto& row, auto value) { return row.bus < value; });
    if (it != rows.end() && it->bus == bus) {
        if (matrix) it->matrix = std::move(matrix);
        else rows.erase(it);
    } else if (matrix) rows.insert(it, {bus, std::move(matrix)});
}

const BusDownmixMatrix* TopologyPlanView::CustomDownmixFor(std::uint32_t bus) const noexcept {
    const auto it = std::lower_bound(customDownmix.begin(), customDownmix.end(), bus,
        [](const auto& row, auto value) { return row.bus < value; });
    return it != customDownmix.end() && it->bus == bus ? it->matrix.get() : nullptr;
}

void TopologyPlanView::CopyDownmix(std::uint32_t bus, float* destination) const noexcept {
    if (const auto* custom = CustomDownmixFor(bus))
        std::copy_n(custom->data(), 64, destination);
    else
        DefaultDownmixMatrix(mixLayout, outputLayouts ? outputLayouts[bus] : 0, destination);
}

std::uint32_t* TopologyPlanPool::ParentsData(std::uint32_t planIndex) {
    return m_storage[planIndex].parents.data();
}

std::uint8_t* TopologyPlanPool::AliveData(std::uint32_t planIndex) {
    return m_storage[planIndex].alive.data();
}

VnBusId* TopologyPlanPool::BusIdsData(std::uint32_t planIndex) {
    return m_storage[planIndex].busIds.data();
}

std::uint32_t* TopologyPlanPool::GenerationsData(
    std::uint32_t planIndex) {
    return m_storage[planIndex].busGenerations.data();
}

std::uint32_t* TopologyPlanPool::MigrationTargetsData(
    std::uint32_t planIndex) {
    return m_storage[planIndex].migrationTargets.data();
}

float* TopologyPlanPool::PreparedBusVolumeDbData(
    std::uint32_t planIndex) {
    return m_storage[planIndex].preparedBusVolumeDb.data();
}

float* TopologyPlanPool::PreparedBusOutputVolumeDbData(
    std::uint32_t planIndex) {
    return m_storage[planIndex].preparedBusOutputVolumeDb.data();
}

std::uint32_t* TopologyPlanPool::PreparedBusVolumeRampMsData(
    std::uint32_t planIndex) {
    return m_storage[planIndex].preparedBusVolumeRampMs.data();
}

std::uint8_t* TopologyPlanPool::BusVolumeApplyData(
    std::uint32_t planIndex) {
    return m_storage[planIndex].busVolumeApply.data();
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
    v.order = m_storage[planIndex].order.data();
    v.orderCount = m_orderCount[planIndex];
    const auto& sends = m_storage[planIndex];
    v.sendRanges = sends.ranges.data();
    v.sendSlots = sends.slots.data();
    v.sendGenerations = sends.generations.data();
    v.sendTargets = sends.targets.data();
    v.preparedSendLevels = sends.levels.data();
    v.sendLevelApply = sends.apply.data();
    v.sendLevelRampMs = sends.ramps.data();
    v.sendCount = static_cast<std::uint32_t>(sends.targets.size());
    v.sendStates = sends.states.data();
    v.slotsPerBus = m_slotsPerBus;
    v.outputLayouts = m_storage[planIndex].outputLayouts.data();
    v.customDownmix = m_storage[planIndex].customDownmix;
    v.mixLayout = m_storage[planIndex].mixLayout;
    v.parents = m_storage[planIndex].parents.data();
    v.alive = m_storage[planIndex].alive.data();
    v.busIds = m_storage[planIndex].busIds.data();
    v.generations = m_storage[planIndex].busGenerations.data();
    v.migrationTargets =
        m_storage[planIndex].migrationTargets.data();
    v.preparedBusVolumeDb =
        m_storage[planIndex].preparedBusVolumeDb.data();
    v.preparedBusOutputVolumeDb =
        m_storage[planIndex].preparedBusOutputVolumeDb.data();
    v.preparedBusVolumeRampMs =
        m_storage[planIndex].preparedBusVolumeRampMs.data();
    v.busVolumeApply = m_storage[planIndex].busVolumeApply.data();
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
