#ifndef HARKEN_RUNTIME_API_HARKEN_API_INTERNAL_H
#define HARKEN_RUNTIME_API_HARKEN_API_INTERNAL_H

#include <harken/Harken.h>

#include <cstddef>

#include "PlayableRuntime.h"
#include "harken/core/QueueRuntime.h"
#include "harken/core/PlayablePropertyCut.h"
#include "harken/core/TimelineProgram.h"
#include "harken/core/TimelineVoiceInstallInput.h"

namespace harken {

struct Engine;
class PreparedQueueReplacementOrder;
struct PackPartitionLease;
class TopologyPlanPool;
struct ProspectiveTopologyView;

void FillLiveOrPreparedPlan(Engine& engine, TopologyPlanPool& plans,
                            std::uint32_t planIndex,
                            const ProspectiveTopologyView* prospective = nullptr);
bool ProspectiveTopologyIsAcyclic(
    Engine& engine, const ProspectiveTopologyView* prospective);
bool AdmitSingleLiveCommand(Engine& engine);
void ClearPairedInstallAckIdentity(Engine& engine, std::uint32_t planIndex);
void UpdateSendHighWater(Engine& engine) noexcept;
void ResetBusLogicSlot(Engine& engine, std::uint32_t busIndex,
                       std::uint32_t retiringGeneration,
                       std::uint32_t newGeneration) noexcept;

}

namespace harken::test {

struct TimelineVoiceInstallResult {
    VnResult status = VN_INVALID_ARG;
    VnVoiceHandle renderHandle = 0;
};

PlayableStartStatus BuildTimelineVoiceInstallFromCut(
    Engine& engine, const ResolvedPlayablePropertyCutViewV1& cut,
    TimelineVoiceInstallDesc* out,
    const PackPartitionLease* leaseOrNull = nullptr) noexcept;

VnResult PreflightTimelineVoiceStart(
    const Engine& engine, const TimelineVoiceInstallDesc& install) noexcept;

VnResult PreflightTimelineVoiceStart(
    const Engine& engine, const TimelineVoiceInstallInput& input) noexcept;

TimelineVoiceInstallResult InstallTimelineVoiceStart(
    Engine& engine, const TimelineVoiceInstallDesc& install, ProgramGeneration programGeneration = 0,
    PlayableInstanceHandle parentInstance = 0, PlayableInstanceHandle playableRoot = 0);

TimelineVoiceInstallResult InstallTimelineVoiceStart(
    Engine& engine, const TimelineVoiceInstallInput& input, ProgramGeneration programGeneration = 0,
    PlayableInstanceHandle parentInstance = 0, PlayableInstanceHandle playableRoot = 0);

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
bool PrepareNativeQueueResetResourcesForTest(Engine& engine, std::uint32_t serviceSlot,
    std::uint32_t scratchSlot, const QueueInstanceTable::PreparedCatalogResetScope& scope,
    const PreparedQueueReplacementOrder& order) noexcept;
bool CancelNativeQueueResetResourcesForTest(Engine& engine, std::uint32_t scratchSlot) noexcept;
void AbandonNativeQueueResetResourcesForTest(Engine& engine, std::uint32_t count) noexcept;
bool StageInitialQueueSuccessorMirrorsForTest(Engine& engine, VnActionFiringHandle firing,
    std::size_t programCount) noexcept;
#endif

bool CollectInitialQueueTimingMembersForTest(
    Engine& engine, VnActionFiringHandle firing,
    std::uint64_t finalEntryFrame, std::size_t* memberCount,
    std::size_t* programCount, std::size_t* eventCount,
    std::size_t* leafCount, std::size_t* groupCount,
    std::size_t* holderCount) noexcept;

}

namespace harken::monitor { class RequestObservation; }

namespace harken::detail {

bool CanonicalSetMutationBlocked() noexcept;

MonitorRequestContext BeginMonitorRequest(
    MonitorRequestOrigin origin, std::uint32_t operation) noexcept;

VnResult PlayVoiceRaw(const VnPlayDesc* desc,
                      VnVoiceHandle* outHandle) noexcept;
VnResult PlayVoiceRaw(const VnPlayDesc* desc, VnVoiceHandle* outHandle,
                     const MonitorRequestContext& request, monitor::RequestObservation* observation = nullptr) noexcept;
void CaptureMonitorAcceptedRequest(monitor::RequestObservation& observation,
    std::uint32_t subject, std::uint64_t handle) noexcept;
bool DirectWorldHasActionProgram(VnActionId actionId) noexcept;

}

#endif
