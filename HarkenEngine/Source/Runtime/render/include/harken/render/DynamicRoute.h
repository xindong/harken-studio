// Effect-private waveform route storage, immutable audio plans, and bounded
// Bus/Voice tap banks. Sample storage is prepared before acquiring a tap. Logic
// owns route definitions, tap references, plan construction, and retirement;
// audio owns the installed plan, block banks, detector scratch, and status
// publication.
#ifndef HARKEN_RENDER_DYNAMIC_ROUTE_H
#define HARKEN_RENDER_DYNAMIC_ROUTE_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/core/EngineConfig.h"
#include "harken/core/PinnedSnapshot3.h"
#include "harken/core/StableRowStorage.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace harken {

inline constexpr std::uint32_t kInvalidDynamicRoutePlanIndex = 0xFFFFFFFFu;
inline constexpr std::uint32_t kInvalidDynamicTapSlot = 0xFFFFFFFFu;
inline constexpr std::uint32_t kDynamicRouteSlotsPerEffect = 4;

struct DynamicRouteAudioStatusPayload {
    std::uint64_t routeGeneration = 0;
    std::uint64_t routeRevision = 0;
    std::uint64_t blockEpoch = 0;
    std::uint32_t stateFlags = 0;
    std::uint32_t inactiveReason =
        VN_DYNAMIC_ROUTE_INACTIVE_NO_AUDIO_STATUS;
};

struct DynamicRouteDefinition {
    bool active = false;
    std::uint32_t tableIndex = 0;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::uint64_t pendingPlanRevision = 0;
    VnBusId targetBusId{};
    std::uint32_t targetBusIndex = 0;
    std::uint32_t targetEffectType = VN_EFFECT_COMPRESSOR;
    std::uint32_t effectSlot = 0;
    std::uint32_t routeSlot = 0;
    std::uint32_t signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    std::uint32_t sourceKind = 0;
    std::uint32_t tap = 0;
    VnObjectId sourceId{};
    VnVoiceHandle sourceVoiceHandle = 0;
    std::uint32_t sourceIndex = 0;
    std::uint32_t sourceGeneration = 0;
    float levelDb = 0.0f;
    float levelLinear = 1.0f;
    std::uint32_t tapSlot = kInvalidDynamicTapSlot;
    std::uint64_t tapGeneration = 0;
    std::uint32_t ownerKind = VN_DYNAMIC_ROUTE_OWNER_CALLER;
    VnPackHandle ownerCookie = 0;
    bool armFirstBlock = false;
    bool operator==(const DynamicRouteDefinition&) const noexcept = default;
};

struct DynamicRoutePlanRow {
    std::uint32_t routeTableIndex = 0;
    std::uint64_t routeGeneration = 0;
    std::uint64_t routeRevision = 0;
    std::uint32_t targetBusIndex = 0;
    std::uint32_t targetEffectType = VN_EFFECT_COMPRESSOR;
    std::uint32_t effectSlot = 0;
    std::uint32_t routeSlot = 0;
    std::uint32_t sourceKind = 0;
    std::uint32_t sourceIndex = 0;
    std::uint32_t sourceGeneration = 0;
    std::uint32_t tap = 0;
    std::uint32_t tapSlot = kInvalidDynamicTapSlot;
    std::uint64_t tapGeneration = 0;
    float levelLinear = 1.0f;
    bool armFirstBlock = false;
};

struct DynamicRoutePlanView {
    const DynamicRoutePlanRow* rows = nullptr;
    std::uint32_t rowCount = 0;
    std::uint64_t revision = 0;
    bool valid = false;
};

struct DynamicDetectorStatus {
    bool authored = false;
    bool active = false;
    std::uint32_t staleRouteCount = 0;
    std::uint64_t nonFiniteCount = 0;
    std::uint64_t saturationCount = 0;
};

class DynamicRouteSystem {
public:
    explicit DynamicRouteSystem(const EngineConfig& config)
        : m_routeCapacity(config.dynamicRouteCapacity),
          // Public capacity counts mutable plans. Slot zero is an additional
          // permanent empty plan, so a configured minimum of two always leaves
          // one installed mutable plan and one staging replacement.
          m_planCapacity(config.dynamicRoutePlanPoolCapacity + 1),
          m_busTapCapacity(config.dynamicBusTapCapacity),
          m_voiceTapCapacity(config.dynamicVoiceTapCapacity),
          m_blockFrames(config.blockFrames),
          m_routes(std::make_unique<RouteSlot[]>(m_routeCapacity)),
          m_routeGenerations(m_routeCapacity, 0),
          m_busTaps(std::make_unique<TapSlot[]>(m_busTapCapacity)),
          m_voiceTaps(std::make_unique<TapSlot[]>(m_voiceTapCapacity)),
          m_planRows(static_cast<std::size_t>(m_planCapacity) *
                     m_routeCapacity),
          m_planRowCounts(m_planCapacity, 0),
          m_planRevisions(m_planCapacity, 0),
          m_planGenerations(m_planCapacity, 1),
          m_planPins(static_cast<std::size_t>(m_planCapacity) *
                     (m_busTapCapacity + m_voiceTapCapacity),
                     kInvalidDynamicTapSlot),
          m_planPinCounts(m_planCapacity, 0),
          m_detectorScratch(m_blockFrames, 0.0f),
          m_busCaptured(m_busTapCapacity, 0),
          m_voiceWriteCleared(m_voiceTapCapacity, 0),
          m_voiceWriteValid(m_voiceTapCapacity, 0),
          m_voiceReadValid(m_voiceTapCapacity, 0),
          m_firstBlockPending(m_routeCapacity, 0),
          m_firstBlockEvaluated(m_routeCapacity, 0),
          m_targetEvaluated(m_routeCapacity, 0),
          m_sourceSilent(m_routeCapacity, 0),
          m_tapGenerationMismatch(m_routeCapacity, 0),
          m_audioRouteGeneration(m_routeCapacity, 0),
          m_audioRouteRevision(m_routeCapacity, 0) {
        if (!m_busTapSamples.Reserve(static_cast<std::size_t>(m_busTapCapacity) *
                kMaxChannels * m_blockFrames) ||
            !m_voiceTapSamples.Reserve(static_cast<std::size_t>(m_voiceTapCapacity) * 2u *
                kMaxChannels * m_blockFrames)) throw std::bad_alloc{};
        m_routeFree.reserve(m_routeCapacity);
        m_busTapFree.reserve(m_busTapCapacity);
        m_voiceTapFree.reserve(m_voiceTapCapacity);
        for (std::uint32_t i = m_routeCapacity; i-- > 0;) {
            m_routeFree.push_back(i);
            m_routes[i].definition.tableIndex = i;
        }
        for (std::uint32_t i = m_busTapCapacity; i-- > 0;) {
            m_busTapFree.push_back(i);
        }
        for (std::uint32_t i = m_voiceTapCapacity; i-- > 0;) {
            m_voiceTapFree.push_back(i);
        }
        // Slot zero is the permanent empty installed plan.
        for (std::uint32_t i = m_planCapacity; i-- > 1;) {
            m_planFree.push_back(i);
        }
        m_planInUse = 0;
        m_planHighWater = 0;
        const std::uint32_t ringCapacity =
            RoundUpPowerOfTwo(m_planCapacity);
        m_retire.resize(ringCapacity);
        m_ack.resize(ringCapacity);
        m_ringMask = ringCapacity - 1;
    }

    DynamicRouteSystem(const DynamicRouteSystem&) = delete;
    DynamicRouteSystem& operator=(const DynamicRouteSystem&) = delete;

    // --- logic side ---

    void DrainAudioMessages() {
        DrainAcknowledgements();
        const std::uint64_t head =
            m_retireHead.load(std::memory_order_acquire);
        std::uint64_t tail = m_retireTail.load(std::memory_order_relaxed);
        while (tail != head) {
            RecyclePlan(m_retire[tail & m_ringMask]);
            ++tail;
        }
        m_retireTail.store(tail, std::memory_order_release);
    }

    const DynamicRouteDefinition* Find(VnBusId targetBusId,
                                       std::uint32_t effectSlot,
                                       std::uint32_t routeSlot) const {
        for (std::uint32_t i = 0; i < m_routeCapacity; ++i) {
            const DynamicRouteDefinition& row = m_routes[i].definition;
            if (row.active && row.targetBusId == targetBusId &&
                row.effectSlot == effectSlot && row.routeSlot == routeSlot) {
                return &row;
            }
        }
        return nullptr;
    }

    bool HasTargetRoutes(std::uint32_t targetBusIndex,
                         std::uint32_t effectSlot) const {
        for (std::uint32_t i = 0; i < m_routeCapacity; ++i) {
            const DynamicRouteDefinition& row = m_routes[i].definition;
            if (row.active && row.targetBusIndex == targetBusIndex &&
                row.effectSlot == effectSlot) {
                return true;
            }
        }
        return false;
    }

    bool HasRoutesForBusSet(const std::uint8_t* selected,
                            std::uint32_t count) const {
        if (selected == nullptr) {
            return false;
        }
        for (std::uint32_t i = 0; i < m_routeCapacity; ++i) {
            const DynamicRouteDefinition& row = m_routes[i].definition;
            if (!row.active) {
                continue;
            }
            const bool targetSelected =
                row.targetBusIndex < count && selected[row.targetBusIndex] != 0;
            const bool sourceSelected =
                row.sourceKind == VN_DYNAMIC_SOURCE_BUS &&
                row.sourceIndex < count && selected[row.sourceIndex] != 0;
            if (targetSelected || sourceSelected) {
                return true;
            }
        }
        return false;
    }

    std::uint32_t ActiveRouteCount() const {
        return m_activeRouteCount;
    }
    std::uint32_t RouteCapacity() const { return m_routeCapacity; }
    std::uint32_t RouteHighWater() const { return m_routeHighWater; }
    std::uint32_t PlanCapacity() const { return m_planCapacity - 1; }
    std::uint32_t PlanInUse() const { return m_planInUse; }
    std::uint32_t PlanHighWater() const { return m_planHighWater; }
    std::uint32_t BusTapInUse() const { return m_busTapInUse; }
    std::uint32_t BusTapCapacity() const { return m_busTapCapacity; }
    std::uint32_t BusTapHighWater() const { return m_busTapHighWater; }
    std::uint32_t VoiceTapInUse() const { return m_voiceTapInUse; }
    std::uint32_t VoiceTapCapacity() const { return m_voiceTapCapacity; }
    std::uint32_t VoiceTapHighWater() const {
        return m_voiceTapHighWater;
    }
    std::size_t TapSampleStorageBytes() const noexcept {
        return m_busTapSamples.RequestedBytes() + m_voiceTapSamples.RequestedBytes();
    }
#if !defined(HARKEN_RT_QUALIFICATION)
    void SetTapSampleCommitPageLimitsForTests(std::size_t busPages, std::size_t voicePages) noexcept {
        m_busTapSamples.SetCommitPageLimitForTests(busPages);
        m_voiceTapSamples.SetCommitPageLimitForTests(voicePages);
    }
#endif
    std::uint64_t PlanGeneration(std::uint32_t planIndex) const {
        return planIndex < m_planCapacity
                   ? m_planGenerations[planIndex]
                   : 0;
    }
    std::uint64_t LastIssuedPlanRevision() const {
        return m_nextPlanRevision;
    }
    std::uint64_t TapGeneration(std::uint32_t sourceKind,
                                std::uint32_t tapSlot) const {
        const TapSlot* slots =
            sourceKind == VN_DYNAMIC_SOURCE_BUS
                ? m_busTaps.get()
                : m_voiceTaps.get();
        const std::uint32_t capacity =
            sourceKind == VN_DYNAMIC_SOURCE_BUS
                ? m_busTapCapacity
                : m_voiceTapCapacity;
        return tapSlot < capacity ? slots[tapSlot].generation : 0;
    }
    std::uint32_t TapRefCount(std::uint32_t sourceKind,
                              std::uint32_t tapSlot) const {
        const TapSlot* slots =
            sourceKind == VN_DYNAMIC_SOURCE_BUS
                ? m_busTaps.get()
                : m_voiceTaps.get();
        const std::uint32_t capacity =
            sourceKind == VN_DYNAMIC_SOURCE_BUS
                ? m_busTapCapacity
                : m_voiceTapCapacity;
        return tapSlot < capacity ? slots[tapSlot].refCount : 0;
    }
    std::uint64_t RouteRefusalCount() const {
        return m_routeRefusalCount;
    }
    std::uint64_t RouteCycleCount() const { return m_routeCycleCount; }
    std::uint64_t RouteOrderRefusalCount() const {
        return m_routeOrderRefusalCount;
    }
    std::uint64_t StaleReadCount() const { return m_staleReadCount; }
    std::uint64_t InstalledRevision() const { return m_installedRevision; }

    void NoteRouteRefusal() { ++m_routeRefusalCount; }
    void NoteRouteCycle() {
        ++m_routeRefusalCount;
        ++m_routeCycleCount;
    }
    void NoteRouteOrderRefusal() {
        ++m_routeRefusalCount;
        ++m_routeOrderRefusalCount;
    }

    template <typename Fn>
    void ForEachDefinition(Fn&& fn) const {
        for (std::uint32_t i = 0; i < m_routeCapacity; ++i) {
            const DynamicRouteDefinition& row = m_routes[i].definition;
            if (row.active) {
                fn(row);
            }
        }
    }

    bool MatchesDefinition(const DynamicRouteDefinition& expected) const noexcept {
        return expected.active && expected.tableIndex < m_routeCapacity &&
            m_routes[expected.tableIndex].definition == expected;
    }

    bool AllocateRouteSlot(std::uint32_t* outIndex) {
        if (outIndex == nullptr || m_routeFree.empty()) {
            return false;
        }
        *outIndex = m_routeFree.back();
        m_routeFree.pop_back();
        return true;
    }

    void ReleaseUncommittedRouteSlot(std::uint32_t index) {
        if (index < m_routeCapacity) {
            m_routeFree.push_back(index);
        }
    }

    std::uint64_t NextRouteGeneration(std::uint32_t index) const {
        if (index >= m_routeCapacity) {
            return 0;
        }
        std::uint64_t next = m_routeGenerations[index] + 1;
        return next == 0 ? 1 : next;
    }

    bool PrepareBusTapBacking(std::size_t additionalTaps,
                              StableStoragePreparation* out) noexcept {
        const auto samplesPerTap = static_cast<std::size_t>(kMaxChannels) * m_blockFrames;
        const auto samples = (std::min)(additionalTaps,
            static_cast<std::size_t>(m_busTapCapacity)) * samplesPerTap;
        if (samples > UINT32_MAX) return false;
        auto occupied = m_busTapCapacity;
        while (occupied != 0 && !m_busTaps[occupied - 1u].active) --occupied;
        return m_busTapSamples.PrepareBacking(static_cast<std::uint32_t>(samples),
            static_cast<std::size_t>(occupied) * samplesPerTap, out);
    }

    bool AcquireRouteTap(DynamicRouteDefinition& candidate) {
        TapSlot* slots = candidate.sourceKind == VN_DYNAMIC_SOURCE_BUS
                             ? m_busTaps.get()
                             : m_voiceTaps.get();
        const std::uint32_t capacity =
            candidate.sourceKind == VN_DYNAMIC_SOURCE_BUS
                ? m_busTapCapacity
                : m_voiceTapCapacity;
        std::vector<std::uint32_t>& free =
            candidate.sourceKind == VN_DYNAMIC_SOURCE_BUS
                ? m_busTapFree
                : m_voiceTapFree;
        for (std::uint32_t i = 0; i < capacity; ++i) {
            TapSlot& slot = slots[i];
            if (slot.active && slot.sourceKind == candidate.sourceKind &&
                slot.sourceIndex == candidate.sourceIndex &&
                slot.sourceGeneration == candidate.sourceGeneration &&
                slot.tap == candidate.tap) {
                ++slot.refCount;
                candidate.tapSlot = i;
                candidate.tapGeneration = slot.generation;
                return true;
            }
        }
        if (free.empty()) {
            return false;
        }
        const std::uint32_t index = free.back();
        auto& samples = candidate.sourceKind == VN_DYNAMIC_SOURCE_BUS ? m_busTapSamples : m_voiceTapSamples;
        const auto banks = candidate.sourceKind == VN_DYNAMIC_SOURCE_BUS ? 1u : 2u;
        if (!samples.ConstructThrough((static_cast<std::size_t>(index) + 1u) * banks *
                kMaxChannels * m_blockFrames)) return false;
        free.pop_back();
        TapSlot& slot = slots[index];
        slot.active = true;
        slot.sourceKind = candidate.sourceKind;
        slot.sourceIndex = candidate.sourceIndex;
        slot.sourceGeneration = candidate.sourceGeneration;
        slot.tap = candidate.tap;
        ++slot.generation;
        if (slot.generation == 0) {
            ++slot.generation;
        }
        slot.refCount = 1;
        candidate.tapSlot = index;
        candidate.tapGeneration = slot.generation;
        ClearTapStorage(candidate.sourceKind, index);
        if (candidate.sourceKind == VN_DYNAMIC_SOURCE_BUS) {
            ++m_busTapInUse;
            m_busTapHighWater =
                (std::max)(m_busTapHighWater, m_busTapInUse);
        } else {
            ++m_voiceTapInUse;
            m_voiceTapHighWater =
                (std::max)(m_voiceTapHighWater, m_voiceTapInUse);
        }
        return true;
    }

    void ReleaseRouteTap(const DynamicRouteDefinition& row) noexcept {
        ReleaseTap(row.sourceKind, row.tapSlot, row.tapGeneration);
    }

    std::uint32_t AcquirePlan() {
        DrainAudioMessages();
        if (m_planFree.empty()) {
            return kInvalidDynamicRoutePlanIndex;
        }
        const std::uint32_t index = m_planFree.back();
        m_planFree.pop_back();
        ++m_planInUse;
        m_planHighWater = (std::max)(m_planHighWater, m_planInUse);
        ++m_planGenerations[index];
        if (m_planGenerations[index] == 0) {
            ++m_planGenerations[index];
        }
        m_planRowCounts[index] = 0;
        m_planPinCounts[index] = 0;
        m_planRevisions[index] = ++m_nextPlanRevision;
        return index;
    }

    bool BuildPlan(std::uint32_t planIndex,
                   const DynamicRouteDefinition* replacement,
                   bool clearReplacement) {
        if (planIndex == 0 || planIndex >= m_planCapacity) {
            return false;
        }
        std::uint32_t count = 0;
        const auto append = [&](const DynamicRouteDefinition& row) {
            if (count >= m_routeCapacity) {
                return false;
            }
            DynamicRoutePlanRow& out =
                m_planRows[PlanRowBase(planIndex) + count++];
            out.routeTableIndex = row.tableIndex;
            out.routeGeneration = row.generation;
            out.routeRevision = row.revision;
            out.targetBusIndex = row.targetBusIndex;
            out.targetEffectType = row.targetEffectType;
            out.effectSlot = row.effectSlot;
            out.routeSlot = row.routeSlot;
            out.sourceKind = row.sourceKind;
            out.sourceIndex = row.sourceIndex;
            out.sourceGeneration = row.sourceGeneration;
            out.tap = row.tap;
            out.tapSlot = row.tapSlot;
            out.tapGeneration = row.tapGeneration;
            out.levelLinear = row.levelLinear;
            out.armFirstBlock = row.armFirstBlock;
            return PinPlanTap(planIndex, row.sourceKind, row.tapSlot,
                              row.tapGeneration);
        };

        for (std::uint32_t i = 0; i < m_routeCapacity; ++i) {
            const DynamicRouteDefinition& current =
                m_routes[i].definition;
            if (!current.active) {
                continue;
            }
            if (replacement != nullptr &&
                current.tableIndex == replacement->tableIndex) {
                if (!clearReplacement && !append(*replacement)) {
                    RecyclePlan(planIndex);
                    return false;
                }
            } else if (!append(current)) {
                RecyclePlan(planIndex);
                return false;
            }
        }
        if (replacement != nullptr && !clearReplacement &&
            !m_routes[replacement->tableIndex].definition.active) {
            if (!append(*replacement)) {
                RecyclePlan(planIndex);
                return false;
            }
        }
        DynamicRoutePlanRow* begin =
            m_planRows.data() + PlanRowBase(planIndex);
        std::sort(begin, begin + count,
                  [](const DynamicRoutePlanRow& a,
                     const DynamicRoutePlanRow& b) {
                      if (a.targetBusIndex != b.targetBusIndex) {
                          return a.targetBusIndex < b.targetBusIndex;
                      }
                      if (a.effectSlot != b.effectSlot) {
                          return a.effectSlot < b.effectSlot;
                      }
                      return a.routeSlot < b.routeSlot;
                  });
        m_planRowCounts[planIndex] = count;
        return true;
    }

    bool BuildPlanOverlay(
        std::uint32_t planIndex,
        const DynamicRouteDefinition* upserts,
        std::uint32_t upsertCount,
        const std::uint32_t* removals,
        std::uint32_t removalCount) {
        if (planIndex == 0 || planIndex >= m_planCapacity ||
            (upsertCount != 0 && upserts == nullptr) ||
            (removalCount != 0 && removals == nullptr)) {
            return false;
        }
        std::uint32_t count = 0;
        const auto append = [&](const DynamicRouteDefinition& row) {
            if (count >= m_routeCapacity) {
                return false;
            }
            DynamicRoutePlanRow& out =
                m_planRows[PlanRowBase(planIndex) + count++];
            out.routeTableIndex = row.tableIndex;
            out.routeGeneration = row.generation;
            out.routeRevision = row.revision;
            out.targetBusIndex = row.targetBusIndex;
            out.targetEffectType = row.targetEffectType;
            out.effectSlot = row.effectSlot;
            out.routeSlot = row.routeSlot;
            out.sourceKind = row.sourceKind;
            out.sourceIndex = row.sourceIndex;
            out.sourceGeneration = row.sourceGeneration;
            out.tap = row.tap;
            out.tapSlot = row.tapSlot;
            out.tapGeneration = row.tapGeneration;
            out.levelLinear = row.levelLinear;
            out.armFirstBlock = row.armFirstBlock;
            return PinPlanTap(planIndex, row.sourceKind, row.tapSlot,
                              row.tapGeneration);
        };
        const auto removed = [&](std::uint32_t tableIndex) {
            for (std::uint32_t i = 0; i < removalCount; ++i) {
                if (removals[i] == tableIndex) {
                    return true;
                }
            }
            return false;
        };
        const auto replacement = [&](std::uint32_t tableIndex)
            -> const DynamicRouteDefinition* {
            for (std::uint32_t i = 0; i < upsertCount; ++i) {
                if (upserts[i].tableIndex == tableIndex) {
                    return &upserts[i];
                }
            }
            return nullptr;
        };
        for (std::uint32_t i = 0; i < m_routeCapacity; ++i) {
            const DynamicRouteDefinition& current =
                m_routes[i].definition;
            if (!current.active || removed(current.tableIndex)) {
                continue;
            }
            const DynamicRouteDefinition* revised =
                replacement(current.tableIndex);
            if (!append(revised != nullptr ? *revised : current)) {
                RecyclePlan(planIndex);
                return false;
            }
        }
        for (std::uint32_t i = 0; i < upsertCount; ++i) {
            if (!m_routes[upserts[i].tableIndex].definition.active &&
                !append(upserts[i])) {
                RecyclePlan(planIndex);
                return false;
            }
        }
        DynamicRoutePlanRow* begin =
            m_planRows.data() + PlanRowBase(planIndex);
        std::sort(begin, begin + count,
                  [](const DynamicRoutePlanRow& a,
                     const DynamicRoutePlanRow& b) {
                      if (a.targetBusIndex != b.targetBusIndex) {
                          return a.targetBusIndex < b.targetBusIndex;
                      }
                      if (a.effectSlot != b.effectSlot) {
                          return a.effectSlot < b.effectSlot;
                      }
                      return a.routeSlot < b.routeSlot;
                  });
        m_planRowCounts[planIndex] = count;
        return true;
    }

    bool BuildPlanWithoutTarget(std::uint32_t planIndex,
                                std::uint32_t targetBusIndex,
                                std::uint32_t effectSlot) {
        if (planIndex == 0 || planIndex >= m_planCapacity) {
            return false;
        }
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < m_routeCapacity; ++i) {
            const DynamicRouteDefinition& row = m_routes[i].definition;
            if (!row.active ||
                (row.targetBusIndex == targetBusIndex &&
                 row.effectSlot == effectSlot)) {
                continue;
            }
            DynamicRoutePlanRow& out =
                m_planRows[PlanRowBase(planIndex) + count++];
            out.routeTableIndex = row.tableIndex;
            out.routeGeneration = row.generation;
            out.routeRevision = row.revision;
            out.targetBusIndex = row.targetBusIndex;
            out.targetEffectType = row.targetEffectType;
            out.effectSlot = row.effectSlot;
            out.routeSlot = row.routeSlot;
            out.sourceKind = row.sourceKind;
            out.sourceIndex = row.sourceIndex;
            out.sourceGeneration = row.sourceGeneration;
            out.tap = row.tap;
            out.tapSlot = row.tapSlot;
            out.tapGeneration = row.tapGeneration;
            out.levelLinear = row.levelLinear;
            out.armFirstBlock = row.armFirstBlock;
            if (!PinPlanTap(planIndex, row.sourceKind, row.tapSlot,
                            row.tapGeneration)) {
                RecyclePlan(planIndex);
                return false;
            }
        }
        DynamicRoutePlanRow* begin =
            m_planRows.data() + PlanRowBase(planIndex);
        std::sort(begin, begin + count,
                  [](const DynamicRoutePlanRow& a,
                     const DynamicRoutePlanRow& b) {
                      if (a.targetBusIndex != b.targetBusIndex) {
                          return a.targetBusIndex < b.targetBusIndex;
                      }
                      if (a.effectSlot != b.effectSlot) {
                          return a.effectSlot < b.effectSlot;
                      }
                      return a.routeSlot < b.routeSlot;
                  });
        m_planRowCounts[planIndex] = count;
        return true;
    }

    bool BuildPlanWithoutBusSet(std::uint32_t planIndex,
                                const std::uint8_t* selected,
                                std::uint32_t selectedCount) {
        if (planIndex == 0 || planIndex >= m_planCapacity ||
            selected == nullptr) {
            return false;
        }
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < m_routeCapacity; ++i) {
            const DynamicRouteDefinition& row = m_routes[i].definition;
            if (!row.active) {
                continue;
            }
            const bool targetSelected =
                row.targetBusIndex < selectedCount &&
                selected[row.targetBusIndex] != 0;
            const bool sourceSelected =
                row.sourceKind == VN_DYNAMIC_SOURCE_BUS &&
                row.sourceIndex < selectedCount &&
                selected[row.sourceIndex] != 0;
            if (targetSelected || sourceSelected) {
                continue;
            }
            DynamicRoutePlanRow& out =
                m_planRows[PlanRowBase(planIndex) + count++];
            out.routeTableIndex = row.tableIndex;
            out.routeGeneration = row.generation;
            out.routeRevision = row.revision;
            out.targetBusIndex = row.targetBusIndex;
            out.targetEffectType = row.targetEffectType;
            out.effectSlot = row.effectSlot;
            out.routeSlot = row.routeSlot;
            out.sourceKind = row.sourceKind;
            out.sourceIndex = row.sourceIndex;
            out.sourceGeneration = row.sourceGeneration;
            out.tap = row.tap;
            out.tapSlot = row.tapSlot;
            out.tapGeneration = row.tapGeneration;
            out.levelLinear = row.levelLinear;
            out.armFirstBlock = row.armFirstBlock;
            if (!PinPlanTap(planIndex, row.sourceKind, row.tapSlot,
                            row.tapGeneration)) {
                RecyclePlan(planIndex);
                return false;
            }
        }
        DynamicRoutePlanRow* begin =
            m_planRows.data() + PlanRowBase(planIndex);
        std::sort(begin, begin + count,
                  [](const DynamicRoutePlanRow& a,
                     const DynamicRoutePlanRow& b) {
                      if (a.targetBusIndex != b.targetBusIndex) {
                          return a.targetBusIndex < b.targetBusIndex;
                      }
                      if (a.effectSlot != b.effectSlot) {
                          return a.effectSlot < b.effectSlot;
                      }
                      return a.routeSlot < b.routeSlot;
                  });
        m_planRowCounts[planIndex] = count;
        return true;
    }

    void AbandonPlan(std::uint32_t planIndex) {
        if (planIndex != 0 && planIndex < m_planCapacity) {
            RecyclePlan(planIndex);
        }
    }

    std::uint64_t PlanRevision(std::uint32_t planIndex) const {
        return planIndex < m_planCapacity ? m_planRevisions[planIndex] : 0;
    }

    void CommitSet(const DynamicRouteDefinition& candidate) noexcept {
        RouteSlot& slot = m_routes[candidate.tableIndex];
        if (slot.definition.active) {
            ReleaseRouteTap(slot.definition);
        } else {
            ++m_activeRouteCount;
            m_routeHighWater =
                (std::max)(m_routeHighWater, m_activeRouteCount);
        }
        slot.definition = candidate;
        slot.definition.active = true;
        slot.definition.armFirstBlock = false;
        m_routeGenerations[candidate.tableIndex] = candidate.generation;
        slot.lastStatus = DynamicRouteAudioStatusPayload{};
    }

    void CommitClear(std::uint32_t tableIndex,
                     std::uint64_t pendingPlanRevision) noexcept {
        if (tableIndex >= m_routeCapacity) {
            return;
        }
        RouteSlot& slot = m_routes[tableIndex];
        if (!slot.definition.active) {
            return;
        }
        ReleaseRouteTap(slot.definition);
        slot.definition = DynamicRouteDefinition{};
        slot.definition.tableIndex = tableIndex;
        slot.definition.pendingPlanRevision = pendingPlanRevision;
        --m_activeRouteCount;
        m_routeFree.push_back(tableIndex);
    }

    void CommitClearTarget(std::uint32_t targetBusIndex,
                           std::uint32_t effectSlot) {
        for (std::uint32_t i = 0; i < m_routeCapacity; ++i) {
            RouteSlot& slot = m_routes[i];
            if (!slot.definition.active ||
                slot.definition.targetBusIndex != targetBusIndex ||
                slot.definition.effectSlot != effectSlot) {
                continue;
            }
            ReleaseRouteTap(slot.definition);
            slot.definition = DynamicRouteDefinition{};
            slot.definition.tableIndex = i;
            if (m_activeRouteCount > 0) {
                --m_activeRouteCount;
            }
            m_routeFree.push_back(i);
        }
    }

    void CommitClearBusSet(const std::uint8_t* selected,
                           std::uint32_t selectedCount) noexcept {
        if (selected == nullptr) {
            return;
        }
        for (std::uint32_t i = 0; i < m_routeCapacity; ++i) {
            RouteSlot& slot = m_routes[i];
            const DynamicRouteDefinition& row = slot.definition;
            if (!row.active) {
                continue;
            }
            const bool targetSelected =
                row.targetBusIndex < selectedCount &&
                selected[row.targetBusIndex] != 0;
            const bool sourceSelected =
                row.sourceKind == VN_DYNAMIC_SOURCE_BUS &&
                row.sourceIndex < selectedCount &&
                selected[row.sourceIndex] != 0;
            if (!targetSelected && !sourceSelected) {
                continue;
            }
            ReleaseRouteTap(row);
            slot.definition = DynamicRouteDefinition{};
            slot.definition.tableIndex = i;
            if (m_activeRouteCount > 0) {
                --m_activeRouteCount;
            }
            m_routeFree.push_back(i);
        }
    }

    VnResult CaptureRoutes(VnEffectDynamicRouteInfo* out,
                           std::uint32_t capacity,
                           std::uint32_t* requiredCount) {
        DrainAudioMessages();
        if (requiredCount == nullptr ||
            (capacity != 0 && out == nullptr)) {
            return VN_INVALID_ARG;
        }
        *requiredCount = m_activeRouteCount;
        if (capacity != 0 &&
            out[0].structSize != sizeof(VnEffectDynamicRouteInfo)) {
            return VN_INVALID_ARG;
        }
        std::uint32_t written = 0;
        for (std::uint32_t i = 0; i < m_routeCapacity; ++i) {
            RouteSlot& slot = m_routes[i];
            const DynamicRouteDefinition& row = slot.definition;
            if (!row.active) {
                continue;
            }
            if (written >= capacity) {
                continue;
            }
            DynamicRouteAudioStatusPayload status = slot.lastStatus;
            const bool coherent = slot.status.Capture(status);
            if (coherent) {
                slot.lastStatus = status;
            }
            VnEffectDynamicRouteInfo info{};
            info.structSize = sizeof info;
            info.targetBusId = row.targetBusId;
            info.sourceId = row.sourceId;
            info.sourceVoiceHandle = row.sourceVoiceHandle;
            info.revision = row.revision;
            info.targetEffectType = row.targetEffectType;
            info.effectSlot = row.effectSlot;
            info.routeSlot = row.routeSlot;
            info.signalForm = row.signalForm;
            info.sourceKind = row.sourceKind;
            info.tap = row.tap;
            info.levelDb = row.levelDb;
            info.ownerKind = row.ownerKind;
            info.stateFlags = VN_DYNAMIC_ROUTE_AUTHORED;
            if (row.ownerKind == VN_DYNAMIC_ROUTE_OWNER_PACK) {
                info.stateFlags |= VN_DYNAMIC_ROUTE_PACK_OWNED;
            }
            const bool pending =
                row.pendingPlanRevision > m_installedRevision;
            const bool identityMismatch =
                status.routeGeneration != row.generation ||
                status.routeRevision != row.revision;
            if (pending) {
                info.stateFlags |= VN_DYNAMIC_ROUTE_PENDING;
                info.inactiveReason = VN_DYNAMIC_ROUTE_INACTIVE_PENDING;
                info.pendingPublication = 1;
            } else if (identityMismatch) {
                info.stateFlags |= VN_DYNAMIC_ROUTE_STALE;
                info.inactiveReason =
                    VN_DYNAMIC_ROUTE_INACTIVE_NO_AUDIO_STATUS;
            } else {
                info.blockEpoch = status.blockEpoch;
                info.stateFlags |= status.stateFlags;
                info.inactiveReason = status.inactiveReason;
                if (!coherent) {
                    info.stateFlags |= VN_DYNAMIC_ROUTE_STALE;
                }
            }
            if (!pending && (!coherent || identityMismatch)) {
                ++m_staleReadCount;
            }
            out[written++] = info;
        }
        return VN_OK;
    }

    // --- both sides: immutable plan storage ---

    DynamicRoutePlanView View(std::uint32_t index) const {
        DynamicRoutePlanView view;
        if (index >= m_planCapacity) {
            return view;
        }
        view.rows = m_planRows.data() + PlanRowBase(index);
        view.rowCount = m_planRowCounts[index];
        view.revision = m_planRevisions[index];
        view.valid = true;
        return view;
    }

    // --- audio side ---

    void InstallPlan(std::uint32_t index) {
        if (index == kInvalidDynamicRoutePlanIndex ||
            index >= m_planCapacity) {
            return;
        }
        const std::uint32_t previous = m_currentPlanIndex;
        m_currentPlanIndex = index;
        ++m_planPublicationCount;
        const DynamicRoutePlanView plan = View(index);
        for (std::uint32_t i = 0; i < plan.rowCount; ++i) {
            const DynamicRoutePlanRow& row = plan.rows[i];
            if (row.routeTableIndex >= m_routeCapacity) {
                continue;
            }
            const bool identityChanged =
                m_audioRouteGeneration[row.routeTableIndex] !=
                    row.routeGeneration;
            m_audioRouteGeneration[row.routeTableIndex] =
                row.routeGeneration;
            m_audioRouteRevision[row.routeTableIndex] = row.routeRevision;
            if (row.sourceKind == VN_DYNAMIC_SOURCE_VOICE &&
                (row.armFirstBlock || identityChanged)) {
                m_firstBlockPending[row.routeTableIndex] = 1;
            }
        }
        PushAck(index, m_planGenerations[index], plan.revision);
        if (previous != index && previous != 0) {
            PushRetire(previous);
        }
    }

    void BeginBlock() {
        std::fill(m_busCaptured.begin(), m_busCaptured.end(), 0);
        std::fill(m_voiceWriteCleared.begin(),
                  m_voiceWriteCleared.end(), 0);
        std::fill(m_voiceWriteValid.begin(), m_voiceWriteValid.end(), 0);
        std::fill(m_firstBlockEvaluated.begin(),
                  m_firstBlockEvaluated.end(), 0);
        std::fill(m_targetEvaluated.begin(), m_targetEvaluated.end(), 0);
        std::fill(m_sourceSilent.begin(), m_sourceSilent.end(), 0);
        std::fill(m_tapGenerationMismatch.begin(),
                  m_tapGenerationMismatch.end(), 0);
        const DynamicRoutePlanView plan = View(m_currentPlanIndex);
        for (std::uint32_t i = 0; i < plan.rowCount; ++i) {
            const DynamicRoutePlanRow& row = plan.rows[i];
            if (row.sourceKind != VN_DYNAMIC_SOURCE_VOICE ||
                row.tapSlot >= m_voiceTapCapacity ||
                m_voiceWriteCleared[row.tapSlot] != 0) {
                continue;
            }
            float* base = VoiceTapBase(row.tapSlot, m_voiceWriteBank);
            std::memset(base, 0, static_cast<std::size_t>(kMaxChannels) *
                                     m_blockFrames * sizeof(float));
            m_voiceWriteCleared[row.tapSlot] = 1;
        }
    }

    bool HasVoiceTap(VnVoiceHandle sourceIdentity) const {
        const DynamicRoutePlanView plan = View(m_currentPlanIndex);
        for (std::uint32_t i = 0; i < plan.rowCount; ++i) {
            const auto& row = plan.rows[i];
            if (row.sourceKind == VN_DYNAMIC_SOURCE_VOICE &&
                row.sourceIndex == static_cast<std::uint32_t>(sourceIdentity) &&
                row.sourceGeneration == static_cast<std::uint32_t>(sourceIdentity >> 32) &&
                row.tapSlot < m_voiceTapCapacity) {
                return true;
            }
        }
        return false;
    }

    void PublishVoiceTap(VnVoiceHandle sourceIdentity,
                         float* const* channels,
                         std::uint32_t channelCount,
                         std::uint32_t offset, std::uint32_t frames) {
        const std::uint32_t sourceIndex =
            static_cast<std::uint32_t>(sourceIdentity);
        const std::uint32_t sourceGeneration =
            static_cast<std::uint32_t>(sourceIdentity >> 32);
        const DynamicRoutePlanView plan = View(m_currentPlanIndex);
        for (std::uint32_t i = 0; i < plan.rowCount; ++i) {
            const DynamicRoutePlanRow& row = plan.rows[i];
            if (row.sourceKind != VN_DYNAMIC_SOURCE_VOICE ||
                row.sourceIndex != sourceIndex ||
                row.sourceGeneration != sourceGeneration ||
                row.tapSlot >= m_voiceTapCapacity) {
                continue;
            }
            if (!TapMatches(m_voiceTaps[row.tapSlot], row)) {
                if (row.routeTableIndex < m_routeCapacity) {
                    m_tapGenerationMismatch[row.routeTableIndex] = 1;
                }
                ++m_tapGenerationMismatchCount;
                continue;
            }
            if (m_voiceReadValid[row.tapSlot] == 0) {
                if (row.routeTableIndex < m_routeCapacity) {
                    m_firstBlockPending[row.routeTableIndex] = 1;
                }
            }
            if (m_voiceWriteValid[row.tapSlot] != 0) {
                continue;
            }
            if (channels != nullptr && offset < m_blockFrames) {
                float* base = VoiceTapBase(row.tapSlot, m_voiceWriteBank);
                const std::uint32_t count =
                    (std::min)(channelCount, kMaxChannels);
                const std::uint32_t copyFrames =
                    (std::min)(frames, m_blockFrames - offset);
                for (std::uint32_t ch = 0; ch < count; ++ch) {
                    std::memcpy(
                        base + static_cast<std::size_t>(ch) *
                                   m_blockFrames +
                            offset,
                        channels[ch] + offset,
                        static_cast<std::size_t>(copyFrames) *
                            sizeof(float));
                }
            }
            m_voiceWriteValid[row.tapSlot] = 1;
        }
    }

    void InvalidateVoiceTap(VnVoiceHandle sourceIdentity) {
        const std::uint32_t sourceIndex =
            static_cast<std::uint32_t>(sourceIdentity);
        const std::uint32_t sourceGeneration =
            static_cast<std::uint32_t>(sourceIdentity >> 32);
        for (std::uint32_t i = 0; i < m_voiceTapCapacity; ++i) {
            const TapSlot& tap = m_voiceTaps[i];
            if (!tap.active ||
                tap.sourceKind != VN_DYNAMIC_SOURCE_VOICE ||
                tap.sourceIndex != sourceIndex ||
                tap.sourceGeneration != sourceGeneration) {
                continue;
            }
            m_voiceReadValid[i] = 0;
            m_voiceWriteValid[i] = 0;
            std::memset(VoiceTapBase(i, 0), 0,
                        static_cast<std::size_t>(2) * kMaxChannels *
                            m_blockFrames * sizeof(float));
        }
    }

    void CaptureBus(std::uint32_t sourceIndex, std::uint32_t tap,
                    float* const* channels, std::uint32_t channelCount,
                    std::uint32_t frames) {
        const DynamicRoutePlanView plan = View(m_currentPlanIndex);
        for (std::uint32_t i = 0; i < plan.rowCount; ++i) {
            const DynamicRoutePlanRow& row = plan.rows[i];
            if (row.sourceKind != VN_DYNAMIC_SOURCE_BUS ||
                row.sourceIndex != sourceIndex || row.tap != tap ||
                row.tapSlot >= m_busTapCapacity) {
                continue;
            }
            if (!TapMatches(m_busTaps[row.tapSlot], row)) {
                if (row.routeTableIndex < m_routeCapacity) {
                    m_tapGenerationMismatch[row.routeTableIndex] = 1;
                }
                ++m_tapGenerationMismatchCount;
                continue;
            }
            if (m_busCaptured[row.tapSlot] != 0) {
                continue;
            }
            float* base = BusTapBase(row.tapSlot);
            const std::uint32_t count =
                (std::min)(channelCount, kMaxChannels);
            for (std::uint32_t ch = 0; ch < count; ++ch) {
                std::memcpy(base + static_cast<std::size_t>(ch) *
                                       m_blockFrames,
                            channels[ch],
                            static_cast<std::size_t>(frames) * sizeof(float));
            }
            for (std::uint32_t ch = count; ch < kMaxChannels; ++ch) {
                std::memset(base + static_cast<std::size_t>(ch) *
                                       m_blockFrames,
                            0,
                            static_cast<std::size_t>(frames) * sizeof(float));
            }
            m_busCaptured[row.tapSlot] = 1;
        }
    }

    const float* BuildDetector(std::uint32_t targetBusIndex,
                               std::uint32_t effectSlot,
                               std::uint32_t frames,
                               DynamicDetectorStatus* outStatus) {
        const DynamicRoutePlanView plan = View(m_currentPlanIndex);
        DynamicDetectorStatus status{};
        std::fill(m_detectorScratch.begin(),
                  m_detectorScratch.begin() +
                      (std::min)(frames, m_blockFrames),
                  0.0f);
        for (std::uint32_t i = 0; i < plan.rowCount; ++i) {
            const DynamicRoutePlanRow& row = plan.rows[i];
            if (row.targetBusIndex != targetBusIndex ||
                row.effectSlot != effectSlot) {
                continue;
            }
            status.authored = true;
            if (row.routeTableIndex >= m_routeCapacity) {
                continue;
            }
            m_targetEvaluated[row.routeTableIndex] = 1;
            if (row.sourceKind == VN_DYNAMIC_SOURCE_VOICE &&
                m_firstBlockPending[row.routeTableIndex] != 0) {
                m_firstBlockEvaluated[row.routeTableIndex] = 1;
                ++status.staleRouteCount;
                continue;
            }
            const float* source = nullptr;
            bool valid = false;
            bool tapMatches = false;
            if (row.sourceKind == VN_DYNAMIC_SOURCE_BUS &&
                row.tapSlot < m_busTapCapacity) {
                tapMatches = TapMatches(m_busTaps[row.tapSlot], row);
                if (tapMatches) {
                source = BusTapBase(row.tapSlot);
                valid = m_busCaptured[row.tapSlot] != 0;
                }
            } else if (row.sourceKind == VN_DYNAMIC_SOURCE_VOICE &&
                       row.tapSlot < m_voiceTapCapacity) {
                tapMatches = TapMatches(m_voiceTaps[row.tapSlot], row);
                if (tapMatches) {
                source = VoiceTapBase(row.tapSlot, m_voiceReadBank);
                valid = m_voiceReadValid[row.tapSlot] != 0;
                }
            }
            if (!tapMatches) {
                m_tapGenerationMismatch[row.routeTableIndex] = 1;
                ++m_tapGenerationMismatchCount;
                ++status.staleRouteCount;
                continue;
            }
            if (!valid || source == nullptr) {
                ++status.staleRouteCount;
                continue;
            }
            status.active = true;
            bool routeHasSignal = false;
            for (std::uint32_t frame = 0;
                 frame < frames && frame < m_blockFrames; ++frame) {
                float routeMagnitude = 0.0f;
                for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
                    const float sample =
                        source[static_cast<std::size_t>(ch) *
                                   m_blockFrames +
                               frame];
                    if (!std::isfinite(sample)) {
                        ++m_detectorNonFiniteCount;
                        ++status.nonFiniteCount;
                        continue;
                    }
                    const float magnitude =
                        sample < 0.0f ? -sample : sample;
                    if (magnitude > 0.0f) {
                        routeHasSignal = true;
                    }
                    if (magnitude > routeMagnitude) {
                        routeMagnitude = magnitude;
                    }
                }
                const double scaled =
                    static_cast<double>(routeMagnitude) *
                    static_cast<double>(row.levelLinear);
                float finite = 0.0f;
                if (scaled >
                    static_cast<double>((std::numeric_limits<float>::max)())) {
                    finite = (std::numeric_limits<float>::max)();
                    ++m_detectorSaturationCount;
                    ++status.saturationCount;
                } else if (scaled >=
                           static_cast<double>(
                               (std::numeric_limits<float>::min)())) {
                    finite = static_cast<float>(scaled);
                }
                if (finite > m_detectorScratch[frame]) {
                    m_detectorScratch[frame] = finite;
                }
            }
            m_sourceSilent[row.routeTableIndex] =
                routeHasSignal ? 0u : 1u;
        }
        if (outStatus != nullptr) {
            *outStatus = status;
        }
        return status.authored ? m_detectorScratch.data() : nullptr;
    }

    void EndBlock(std::uint64_t blockEpoch) {
        const DynamicRoutePlanView plan = View(m_currentPlanIndex);
        for (std::uint32_t i = 0; i < plan.rowCount; ++i) {
            const DynamicRoutePlanRow& row = plan.rows[i];
            if (row.routeTableIndex >= m_routeCapacity) {
                continue;
            }
            DynamicRouteAudioStatusPayload& payload =
                m_routes[row.routeTableIndex].status.BeginWrite();
            payload = DynamicRouteAudioStatusPayload{};
            payload.routeGeneration = row.routeGeneration;
            payload.routeRevision = row.routeRevision;
            payload.blockEpoch = blockEpoch;
            bool active = false;
            bool tapMatches = false;
            if (row.sourceKind == VN_DYNAMIC_SOURCE_BUS &&
                row.tapSlot < m_busTapCapacity) {
                tapMatches = TapMatches(m_busTaps[row.tapSlot], row);
                active = tapMatches && m_busCaptured[row.tapSlot] != 0;
            } else if (row.sourceKind == VN_DYNAMIC_SOURCE_VOICE &&
                       row.tapSlot < m_voiceTapCapacity) {
                tapMatches = TapMatches(m_voiceTaps[row.tapSlot], row);
                active = tapMatches && m_voiceReadValid[row.tapSlot] != 0;
            }
            if (m_targetEvaluated[row.routeTableIndex] == 0) {
                payload.stateFlags = VN_DYNAMIC_ROUTE_STALE;
                payload.inactiveReason =
                    VN_DYNAMIC_ROUTE_INACTIVE_TARGET_STALE;
            } else if (!tapMatches ||
                       m_tapGenerationMismatch[row.routeTableIndex] != 0) {
                payload.stateFlags = VN_DYNAMIC_ROUTE_STALE;
                payload.inactiveReason =
                    VN_DYNAMIC_ROUTE_INACTIVE_TAP_GENERATION;
            } else if (row.sourceKind == VN_DYNAMIC_SOURCE_VOICE &&
                m_firstBlockPending[row.routeTableIndex] != 0) {
                payload.stateFlags = VN_DYNAMIC_ROUTE_FIRST_BLOCK;
                payload.inactiveReason =
                    VN_DYNAMIC_ROUTE_INACTIVE_FIRST_BLOCK;
            } else if (active) {
                payload.stateFlags = VN_DYNAMIC_ROUTE_ACTIVE;
                if (m_sourceSilent[row.routeTableIndex] != 0) {
                    payload.stateFlags |= VN_DYNAMIC_ROUTE_SOURCE_SILENT;
                }
                payload.inactiveReason = VN_DYNAMIC_ROUTE_INACTIVE_NONE;
            } else {
                payload.stateFlags = VN_DYNAMIC_ROUTE_STALE;
                payload.inactiveReason =
                    VN_DYNAMIC_ROUTE_INACTIVE_SOURCE_STALE;
            }
            m_routes[row.routeTableIndex].status.EndWrite();
        }
        for (std::uint32_t i = 0; i < m_routeCapacity; ++i) {
            if (m_firstBlockEvaluated[i] != 0) {
                m_firstBlockPending[i] = 0;
            }
        }
        m_voiceReadBank = m_voiceWriteBank;
        m_voiceWriteBank ^= 1u;
        m_voiceReadValid.swap(m_voiceWriteValid);
    }

    std::uint64_t DetectorNonFiniteCount() const {
        return m_detectorNonFiniteCount;
    }
    std::uint64_t DetectorSaturationCount() const {
        return m_detectorSaturationCount;
    }
    std::uint64_t AudioTapGenerationMismatchCount() const {
        return m_tapGenerationMismatchCount;
    }
    std::uint64_t AudioPlanPublicationCount() const {
        return m_planPublicationCount;
    }
    std::uint64_t AudioPlanRetireOverflowCount() const {
        return m_retireOverflowCount + m_ackOverflowCount;
    }

private:
    struct RouteSlot {
        DynamicRouteDefinition definition{};
        PinnedSnapshot3<DynamicRouteAudioStatusPayload> status;
        DynamicRouteAudioStatusPayload lastStatus{};
    };

    struct TapSlot {
        bool active = false;
        std::uint32_t sourceKind = 0;
        std::uint32_t sourceIndex = 0;
        std::uint32_t sourceGeneration = 0;
        std::uint32_t tap = 0;
        std::uint64_t generation = 0;
        std::uint32_t refCount = 0;
    };

    struct Ack {
        std::uint32_t index = 0;
        std::uint64_t generation = 0;
        std::uint64_t revision = 0;
    };

    static std::uint32_t RoundUpPowerOfTwo(std::uint32_t value) {
        std::uint32_t result = 1;
        while (result < value) {
            result <<= 1;
        }
        return result < 2 ? 2 : result;
    }

    std::size_t PlanRowBase(std::uint32_t index) const {
        return static_cast<std::size_t>(index) * m_routeCapacity;
    }
    std::size_t PlanPinBase(std::uint32_t index) const {
        return static_cast<std::size_t>(index) *
               (m_busTapCapacity + m_voiceTapCapacity);
    }

    float* BusTapBase(std::uint32_t slot) {
        return m_busTapSamples.data() +
               static_cast<std::size_t>(slot) * kMaxChannels * m_blockFrames;
    }
    const float* BusTapBase(std::uint32_t slot) const {
        return m_busTapSamples.data() +
               static_cast<std::size_t>(slot) * kMaxChannels * m_blockFrames;
    }
    float* VoiceTapBase(std::uint32_t slot, std::uint32_t bank) {
        return m_voiceTapSamples.data() +
               (static_cast<std::size_t>(slot) * 2 + bank) *
                   kMaxChannels * m_blockFrames;
    }
    const float* VoiceTapBase(std::uint32_t slot,
                              std::uint32_t bank) const {
        return m_voiceTapSamples.data() +
               (static_cast<std::size_t>(slot) * 2 + bank) *
                   kMaxChannels * m_blockFrames;
    }

    bool TapMatches(const TapSlot& slot,
                    const DynamicRoutePlanRow& row) const {
        return slot.active && slot.generation == row.tapGeneration &&
               slot.sourceKind == row.sourceKind &&
               slot.sourceIndex == row.sourceIndex &&
               slot.sourceGeneration == row.sourceGeneration &&
               slot.tap == row.tap;
    }

    void ClearTapStorage(std::uint32_t sourceKind,
                         std::uint32_t index) noexcept {
        if (sourceKind == VN_DYNAMIC_SOURCE_BUS) {
            std::memset(BusTapBase(index), 0,
                        static_cast<std::size_t>(kMaxChannels) *
                            m_blockFrames * sizeof(float));
        } else {
            std::memset(VoiceTapBase(index, 0), 0,
                        static_cast<std::size_t>(2) * kMaxChannels *
                            m_blockFrames * sizeof(float));
        }
    }

    bool PinPlanTap(std::uint32_t planIndex, std::uint32_t sourceKind,
                    std::uint32_t tapSlot, std::uint64_t generation) {
        TapSlot* slots = sourceKind == VN_DYNAMIC_SOURCE_BUS
                             ? m_busTaps.get()
                             : m_voiceTaps.get();
        const std::uint32_t capacity =
            sourceKind == VN_DYNAMIC_SOURCE_BUS ? m_busTapCapacity
                                                : m_voiceTapCapacity;
        if (tapSlot >= capacity || !slots[tapSlot].active ||
            slots[tapSlot].generation != generation) {
            return false;
        }
        const std::uint32_t encoded =
            sourceKind == VN_DYNAMIC_SOURCE_VOICE
                ? (tapSlot | 0x80000000u)
                : tapSlot;
        const std::size_t base = PlanPinBase(planIndex);
        for (std::uint32_t i = 0; i < m_planPinCounts[planIndex]; ++i) {
            if (m_planPins[base + i] == encoded) {
                return true;
            }
        }
        const std::uint32_t limit =
            m_busTapCapacity + m_voiceTapCapacity;
        if (m_planPinCounts[planIndex] >= limit) {
            return false;
        }
        m_planPins[base + m_planPinCounts[planIndex]++] = encoded;
        ++slots[tapSlot].refCount;
        return true;
    }

    void ReleaseTap(std::uint32_t sourceKind, std::uint32_t tapSlot,
                    std::uint64_t generation) noexcept {
        TapSlot* slots = sourceKind == VN_DYNAMIC_SOURCE_BUS
                             ? m_busTaps.get()
                             : m_voiceTaps.get();
        const std::uint32_t capacity =
            sourceKind == VN_DYNAMIC_SOURCE_BUS ? m_busTapCapacity
                                                : m_voiceTapCapacity;
        if (tapSlot >= capacity || !slots[tapSlot].active ||
            slots[tapSlot].generation != generation) {
            return;
        }
        TapSlot& slot = slots[tapSlot];
        if (slot.refCount > 0) {
            --slot.refCount;
        }
        if (slot.refCount == 0) {
            slot.active = false;
            ClearTapStorage(sourceKind, tapSlot);
            if (sourceKind == VN_DYNAMIC_SOURCE_BUS) {
                if (m_busTapInUse > 0) {
                    --m_busTapInUse;
                }
                m_busTapFree.push_back(tapSlot);
            } else {
                if (m_voiceTapInUse > 0) {
                    --m_voiceTapInUse;
                }
                m_voiceTapFree.push_back(tapSlot);
            }
        }
    }

    void RecyclePlan(std::uint32_t index) {
        if (index == 0 || index >= m_planCapacity) {
            return;
        }
        const std::size_t base = PlanPinBase(index);
        for (std::uint32_t i = 0; i < m_planPinCounts[index]; ++i) {
            const std::uint32_t encoded = m_planPins[base + i];
            const bool voice = (encoded & 0x80000000u) != 0;
            const std::uint32_t tapSlot = encoded & 0x7FFFFFFFu;
            TapSlot* slots = voice ? m_voiceTaps.get() : m_busTaps.get();
            const std::uint64_t generation =
                slots[tapSlot].generation;
            ReleaseTap(voice ? VN_DYNAMIC_SOURCE_VOICE
                             : VN_DYNAMIC_SOURCE_BUS,
                       tapSlot, generation);
            m_planPins[base + i] = kInvalidDynamicTapSlot;
        }
        m_planPinCounts[index] = 0;
        m_planRowCounts[index] = 0;
        m_planFree.push_back(index);
        if (m_planInUse > 0) {
            --m_planInUse;
        }
    }

    void PushRetire(std::uint32_t index) {
        const std::uint64_t head =
            m_retireHead.load(std::memory_order_relaxed);
        const std::uint64_t tail =
            m_retireTail.load(std::memory_order_acquire);
        if (head - tail >= m_retire.size()) {
            ++m_retireOverflowCount;
            return;
        }
        m_retire[head & m_ringMask] = index;
        m_retireHead.store(head + 1, std::memory_order_release);
    }

    void PushAck(std::uint32_t index, std::uint64_t generation,
                 std::uint64_t revision) {
        const std::uint64_t head =
            m_ackHead.load(std::memory_order_relaxed);
        const std::uint64_t tail =
            m_ackTail.load(std::memory_order_acquire);
        if (head - tail >= m_ack.size()) {
            ++m_ackOverflowCount;
            return;
        }
        m_ack[head & m_ringMask] = Ack{index, generation, revision};
        m_ackHead.store(head + 1, std::memory_order_release);
    }

    void DrainAcknowledgements() {
        const std::uint64_t head = m_ackHead.load(std::memory_order_acquire);
        std::uint64_t tail = m_ackTail.load(std::memory_order_relaxed);
        while (tail != head) {
            const Ack& ack = m_ack[tail & m_ringMask];
            if (ack.index < m_planCapacity &&
                ack.generation == m_planGenerations[ack.index]) {
                m_installedRevision =
                    (std::max)(m_installedRevision, ack.revision);
            }
            ++tail;
        }
        m_ackTail.store(tail, std::memory_order_release);
    }

    std::uint32_t m_routeCapacity = 0;
    std::uint32_t m_planCapacity = 0;
    std::uint32_t m_busTapCapacity = 0;
    std::uint32_t m_voiceTapCapacity = 0;
    std::uint32_t m_blockFrames = 0;
    std::unique_ptr<RouteSlot[]> m_routes;
    std::vector<std::uint64_t> m_routeGenerations;
    std::unique_ptr<TapSlot[]> m_busTaps;
    std::unique_ptr<TapSlot[]> m_voiceTaps;
    std::vector<std::uint32_t> m_routeFree;
    std::vector<std::uint32_t> m_busTapFree;
    std::vector<std::uint32_t> m_voiceTapFree;
    std::uint32_t m_activeRouteCount = 0;
    std::uint32_t m_routeHighWater = 0;
    std::uint32_t m_busTapInUse = 0;
    std::uint32_t m_busTapHighWater = 0;
    std::uint32_t m_voiceTapInUse = 0;
    std::uint32_t m_voiceTapHighWater = 0;
    std::uint64_t m_routeRefusalCount = 0;
    std::uint64_t m_routeCycleCount = 0;
    std::uint64_t m_routeOrderRefusalCount = 0;
    std::uint64_t m_staleReadCount = 0;

    std::vector<DynamicRoutePlanRow> m_planRows;
    std::vector<std::uint32_t> m_planRowCounts;
    std::vector<std::uint64_t> m_planRevisions;
    std::vector<std::uint64_t> m_planGenerations;
    std::vector<std::uint32_t> m_planPins;
    std::vector<std::uint32_t> m_planPinCounts;
    std::vector<std::uint32_t> m_planFree;
    std::uint32_t m_planInUse = 0;
    std::uint32_t m_planHighWater = 0;
    std::uint64_t m_nextPlanRevision = 0;
    std::uint64_t m_installedRevision = 0;
    std::uint32_t m_currentPlanIndex = 0;

    StableRowStorage<float, std::size_t> m_busTapSamples;
    StableRowStorage<float, std::size_t> m_voiceTapSamples;
    std::vector<float> m_detectorScratch;
    std::vector<std::uint8_t> m_busCaptured;
    std::vector<std::uint8_t> m_voiceWriteCleared;
    std::vector<std::uint8_t> m_voiceWriteValid;
    std::vector<std::uint8_t> m_voiceReadValid;
    std::uint32_t m_voiceWriteBank = 1;
    std::uint32_t m_voiceReadBank = 0;

    std::vector<std::uint8_t> m_firstBlockPending;
    std::vector<std::uint8_t> m_firstBlockEvaluated;
    std::vector<std::uint8_t> m_targetEvaluated;
    std::vector<std::uint8_t> m_sourceSilent;
    std::vector<std::uint8_t> m_tapGenerationMismatch;
    std::vector<std::uint64_t> m_audioRouteGeneration;
    std::vector<std::uint64_t> m_audioRouteRevision;
    std::uint64_t m_detectorNonFiniteCount = 0;
    std::uint64_t m_detectorSaturationCount = 0;
    std::uint64_t m_tapGenerationMismatchCount = 0;
    std::uint64_t m_planPublicationCount = 0;

    std::vector<std::uint32_t> m_retire;
    std::vector<Ack> m_ack;
    std::uint32_t m_ringMask = 0;
    std::atomic<std::uint64_t> m_retireHead{0};
    std::atomic<std::uint64_t> m_retireTail{0};
    std::atomic<std::uint64_t> m_ackHead{0};
    std::atomic<std::uint64_t> m_ackTail{0};
    std::uint64_t m_retireOverflowCount = 0;
    std::uint64_t m_ackOverflowCount = 0;
};

} // namespace harken

#endif // HARKEN_RENDER_DYNAMIC_ROUTE_H
