#pragma once

#include <cstddef>
#include <new>

namespace harken::detail {

// Engine-internal logic-thread test seam; public headers and generated
// bindings must not include this header.
enum class LogicAllocSite : unsigned int {
    None = 0,
    DestroyBusDetaches,
    AudibilityRows,
    AudibilityParked,
    AudibilityCandidates,
    ParkCaptureStorage,
    ContainerResolve,
    ActionCyclePath,
    VoiceEffectParked,
    CurrentWorldArtifactBody,
    CurrentWorldDecodedImage,
    PackVariablePublication,
    ProjectGlobalProjection,
    ProjectEmitterProjection,
    ProjectAdmissionSnapshot,
    ProjectTopologyCopy,
    ProjectCommandReserve,
    ProjectFinalPreflight,
    ProjectTraceProjection,
    CapturedVoiceInstall,
    CapturedScheduleVoices,
    VariableReadCutColumns,
    EffectExtraStorage,
    ScheduledGroupMembers,
    MonitorDerivedStorage,
    ImmediateEntryStorage,
    RetargetIncomingStorage,
    VariableValueStorage,
    CurrentWorldViews,
};

struct LogicAllocFaultState {
    std::size_t remaining = 0;
    LogicAllocSite site = LogicAllocSite::None;
};

inline thread_local LogicAllocFaultState g_logicAllocFault{};

inline void LogicAllocCheckpoint(LogicAllocSite site) {
    auto& fault = g_logicAllocFault;
    if (fault.remaining != 0 &&
        (fault.site == LogicAllocSite::None || fault.site == site) &&
        --fault.remaining == 0) {
        throw std::bad_alloc();
    }
}

template <typename Container>
inline void CheckedGrow(Container& container, std::size_t requested,
                        LogicAllocSite site) {
    if (requested > container.capacity()) {
        LogicAllocCheckpoint(site);
    }
}

inline void ArmLogicAllocFaultForTests(std::size_t nth,
                                       LogicAllocSite site) noexcept {
    g_logicAllocFault.remaining = nth;
    g_logicAllocFault.site = site;
}

inline void DisarmLogicAllocFaultForTests() noexcept {
    g_logicAllocFault = {};
}

} // namespace harken::detail
