#pragma once

#include "harken/core/detail/PackDecodeBudget.h"

#include <cstddef>
#include <cstdint>
#include <new>

namespace harken::detail {

// Engine-internal logic-thread test seam; public headers and generated
// bindings must not include this header.
enum class LogicAllocSite : unsigned int {
    None = 0,
    DestroyBusDetaches,
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
    EffectInstanceStorage,
    EffectRetireStorage,
    ScheduledGroupMembers,
    MonitorDerivedStorage,
    ImmediateEntryStorage,
    RetargetIncomingStorage,
    VariableValueStorage,
    CurrentWorldViews,
    PlayableServiceFeatures,
    SourceCommandBacking,
    PrivateVcLaneStorage,
    PlaybackPathStorage,
    VariableControlRetargetStorage,
    ActionPlayContainerStorage,
    InitialFrontierEvents,
    InitialFrontierRequests,
    PlayableTreeStorage,
    FrontierOwnerLedgerStorage,
    BusSendStorage,
    VoiceSendStorage,
    BusOwnerStorage,
    BusDefinitionStorage,
    BusTargetSettings,
    ApiEntry,
    EngineStorage,
    PackStorage,
    BindingStorage,
    StreamStorage,
    MonitorStorage,
};

// Each public call owns its failure state, including callback reentry. Internal
// calls communicate failures through their result rather than leaking state.
inline const char* LogicAllocSiteName(LogicAllocSite site) noexcept {
    switch (site) {
    case LogicAllocSite::None: return "None";
    case LogicAllocSite::DestroyBusDetaches: return "DestroyBusDetaches";
    case LogicAllocSite::AudibilityParked: return "AudibilityParked";
    case LogicAllocSite::AudibilityCandidates: return "AudibilityCandidates";
    case LogicAllocSite::ParkCaptureStorage: return "ParkCaptureStorage";
    case LogicAllocSite::ContainerResolve: return "ContainerResolve";
    case LogicAllocSite::ActionCyclePath: return "ActionCyclePath";
    case LogicAllocSite::VoiceEffectParked: return "VoiceEffectParked";
    case LogicAllocSite::CurrentWorldArtifactBody: return "CurrentWorldArtifactBody";
    case LogicAllocSite::CurrentWorldDecodedImage: return "CurrentWorldDecodedImage";
    case LogicAllocSite::PackVariablePublication: return "PackVariablePublication";
    case LogicAllocSite::ProjectGlobalProjection: return "ProjectGlobalProjection";
    case LogicAllocSite::ProjectEmitterProjection: return "ProjectEmitterProjection";
    case LogicAllocSite::ProjectAdmissionSnapshot: return "ProjectAdmissionSnapshot";
    case LogicAllocSite::ProjectTopologyCopy: return "ProjectTopologyCopy";
    case LogicAllocSite::ProjectCommandReserve: return "ProjectCommandReserve";
    case LogicAllocSite::ProjectFinalPreflight: return "ProjectFinalPreflight";
    case LogicAllocSite::ProjectTraceProjection: return "ProjectTraceProjection";
    case LogicAllocSite::CapturedVoiceInstall: return "CapturedVoiceInstall";
    case LogicAllocSite::CapturedScheduleVoices: return "CapturedScheduleVoices";
    case LogicAllocSite::VariableReadCutColumns: return "VariableReadCutColumns";
    case LogicAllocSite::EffectExtraStorage: return "EffectExtraStorage";
    case LogicAllocSite::EffectInstanceStorage: return "EffectInstanceStorage";
    case LogicAllocSite::EffectRetireStorage: return "EffectRetireStorage";
    case LogicAllocSite::ScheduledGroupMembers: return "ScheduledGroupMembers";
    case LogicAllocSite::MonitorDerivedStorage: return "MonitorDerivedStorage";
    case LogicAllocSite::ImmediateEntryStorage: return "ImmediateEntryStorage";
    case LogicAllocSite::RetargetIncomingStorage: return "RetargetIncomingStorage";
    case LogicAllocSite::VariableValueStorage: return "VariableValueStorage";
    case LogicAllocSite::CurrentWorldViews: return "CurrentWorldViews";
    case LogicAllocSite::PlayableServiceFeatures: return "PlayableServiceFeatures";
    case LogicAllocSite::SourceCommandBacking: return "SourceCommandBacking";
    case LogicAllocSite::PrivateVcLaneStorage: return "PrivateVcLaneStorage";
    case LogicAllocSite::PlaybackPathStorage: return "PlaybackPathStorage";
    case LogicAllocSite::VariableControlRetargetStorage: return "VariableControlRetargetStorage";
    case LogicAllocSite::InitialFrontierEvents: return "InitialFrontierEvents";
    case LogicAllocSite::InitialFrontierRequests: return "InitialFrontierRequests";
    case LogicAllocSite::ActionPlayContainerStorage: return "ActionPlayContainerStorage";
    case LogicAllocSite::PlayableTreeStorage: return "PlayableTreeStorage";
    case LogicAllocSite::FrontierOwnerLedgerStorage: return "FrontierOwnerLedgerStorage";
    case LogicAllocSite::BusSendStorage: return "BusSendStorage";
    case LogicAllocSite::VoiceSendStorage: return "VoiceSendStorage";
    case LogicAllocSite::BusOwnerStorage: return "BusOwnerStorage";
    case LogicAllocSite::BusDefinitionStorage: return "BusDefinitionStorage";
    case LogicAllocSite::BusTargetSettings: return "BusTargetSettings";
    case LogicAllocSite::ApiEntry: return "ApiEntry";
    case LogicAllocSite::EngineStorage: return "EngineStorage";
    case LogicAllocSite::PackStorage: return "PackStorage";
    case LogicAllocSite::BindingStorage: return "BindingStorage";
    case LogicAllocSite::StreamStorage: return "StreamStorage";
    case LogicAllocSite::MonitorStorage: return "MonitorStorage";
    }
    return "Unknown";
}

struct LogicAllocFailure {
    LogicAllocSite site = LogicAllocSite::None;
    std::uint64_t object = 0;
};

inline thread_local LogicAllocFailure* g_logicAllocCall = nullptr;

class LogicAllocCallScope {
public:
    LogicAllocCallScope() noexcept : previous_(g_logicAllocCall) { g_logicAllocCall = &failure_; }
    ~LogicAllocCallScope() { g_logicAllocCall = previous_; }
    LogicAllocCallScope(const LogicAllocCallScope&) = delete;
    LogicAllocCallScope& operator=(const LogicAllocCallScope&) = delete;
    const LogicAllocFailure& Failure() const noexcept { return failure_; }
private:
    LogicAllocFailure failure_{};
    LogicAllocFailure* previous_;
};

// Preserve existing bad_alloc catch/rollback paths without claiming that a
// boolean preparation refusal (quota, generation or state) exhausted memory.
// A physical failure underneath still records its own allocation evidence.
class LogicPreparationFailure final : public std::bad_alloc {};

class InjectedLogicAllocFailure final : public std::bad_alloc {
public:
    explicit InjectedLogicAllocFailure(LogicAllocSite value) noexcept : site(value) {}
    LogicAllocSite site;
};

inline void RecordLogicAllocFailure(LogicAllocSite site, std::uint64_t object = 0) noexcept {
    if (g_logicAllocCall != nullptr && g_logicAllocCall->site == LogicAllocSite::None)
        *g_logicAllocCall = {site, object};
}

inline void RecordLogicAllocFailure(const std::bad_alloc& failure,
    LogicAllocSite fallback, std::uint64_t object = 0) noexcept {
    // An explicit Pack budget refusal is not a physical allocation failure.
    if (dynamic_cast<const PackDecodeCapacity*>(&failure) != nullptr ||
        dynamic_cast<const LogicPreparationFailure*>(&failure) != nullptr ||
        dynamic_cast<const std::bad_array_new_length*>(&failure) != nullptr) return;
    const auto* injected = dynamic_cast<const InjectedLogicAllocFailure*>(&failure);
    RecordLogicAllocFailure(injected != nullptr ? injected->site : fallback, object);
}

// Call only from an active catch handler. Keep that handler's rollback and
// non-allocation error semantics intact.
inline void RecordCurrentLogicAllocFailure(LogicAllocSite site) noexcept {
    try { throw; }
    catch (const std::bad_alloc& failure) { RecordLogicAllocFailure(failure, site); }
    catch (...) {}
}

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
        throw InjectedLogicAllocFailure(site);
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
