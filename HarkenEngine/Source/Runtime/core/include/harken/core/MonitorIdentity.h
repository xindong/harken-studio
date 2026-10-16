#ifndef HARKEN_CORE_MONITOR_IDENTITY_H
#define HARKEN_CORE_MONITOR_IDENTITY_H

#include <harken/ObjectId.h>
#include "harken/core/CurrentWorld.h"
#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/TimeCoordinates.h"

#include <cstdint>

namespace harken {

inline constexpr std::uint32_t kMonitorIdentityHelperVersion = 1;

inline bool MonitorAcceptsHelperVersion(std::uint32_t helperVersion) noexcept {
    return helperVersion == kMonitorIdentityHelperVersion;
}

struct MonitorInstanceKey {
    std::uint64_t handle = 0;
    std::uint64_t generation = 0;

    bool Valid() const noexcept { return handle != 0 && generation != 0; }
};

struct MonitorContentIdentity {
    VnObjectId projectId{};
    VnObjectId stableId{};
    std::uint64_t worldGeneration = 0;
    std::uint64_t programSemanticFingerprint = 0;
    std::uint64_t contentRevision = 0;
};

struct MonitorOccurrenceIdentity {
    std::uint64_t arrangingInstance = 0;
    VnObjectId clipId{};
    std::uint32_t loopIteration = 0;
    std::uint32_t occurrenceOrdinal = 0;
    bool valid = false;
    bool loopKnown = false;
};

inline MonitorContentIdentity MakeMonitorContentIdentity(
    VnObjectId projectId, std::uint64_t worldGeneration,
    const PlayableRefV1& content) noexcept {
    return {projectId, content.stableId, worldGeneration,
            content.expectedProgramSemanticFingerprint, content.contentRevision};
}

inline bool SameMonitorInstanceKey(const MonitorInstanceKey& a,
                                   const MonitorInstanceKey& b) noexcept {
    return a.handle == b.handle && a.generation == b.generation;
}

inline bool ExecuteFrameInWindow(ExecuteFrame frame, ExecuteFrame begin,
                                 ExecuteFrame endExclusive) noexcept {
    return frame.value >= begin.value && frame.value < endExclusive.value;
}

enum class MonitorWorldAvailability : std::uint8_t {
    Known = 1,
    Unavailable = 2,
};

struct MonitorIdentityFacts {
    MonitorInstanceKey key{};
    MonitorWorldAvailability worldAvailability = MonitorWorldAvailability::Unavailable;
    WorldGeneration world = kNoWorldGeneration;
    std::uint64_t programSemanticFingerprint = 0;
    std::uint64_t runtimeLineageFingerprint = 0;
    std::uint64_t optionalSymbolFingerprint = 0;
    std::uint64_t renderPlanGeneration = 0;
    std::uint64_t clockAnchorGeneration = 0;
    std::uint64_t arrangingInstance = 0;
    VnObjectId clipId{};
    std::uint32_t loopIteration = 0;
    std::uint32_t occurrenceOrdinal = 0;
    LogicCommitSequence logicCommitSequence = 0;
    ExecuteFrame executeFrame{};
};

struct MonitorIdentityDecision {
    bool replaceInstanceKey = false;
    bool updateProgramFingerprint = false;
    bool updateLineageFingerprint = false;
    bool updateSymbolFingerprint = false;
    bool updateRenderPlanGeneration = false;
    bool updateClockAnchorGeneration = false;
};

inline MonitorIdentityDecision ClassifyMonitorIdentityChange(
    const MonitorIdentityFacts& previous,
    const MonitorIdentityFacts& next) noexcept {
    MonitorIdentityDecision decision{};
    if (previous.worldAvailability == MonitorWorldAvailability::Known &&
        next.worldAvailability == MonitorWorldAvailability::Known &&
        previous.world != kNoWorldGeneration &&
        next.world != kNoWorldGeneration && previous.world != next.world) {
        decision.replaceInstanceKey = true;
        decision.updateProgramFingerprint = true;
        decision.updateLineageFingerprint = true;
        decision.updateSymbolFingerprint = true;
        decision.updateRenderPlanGeneration = true;
        decision.updateClockAnchorGeneration = true;
        return decision;
    }
    if (previous.programSemanticFingerprint != next.programSemanticFingerprint) {
        decision.updateProgramFingerprint = true;
    }
    if (previous.runtimeLineageFingerprint != next.runtimeLineageFingerprint) {
        decision.updateLineageFingerprint = true;
    }
    if (previous.optionalSymbolFingerprint != next.optionalSymbolFingerprint) {
        decision.updateSymbolFingerprint = true;
    }
    if (previous.renderPlanGeneration != next.renderPlanGeneration) {
        decision.updateRenderPlanGeneration = true;
    }
    if (previous.clockAnchorGeneration != next.clockAnchorGeneration) {
        decision.updateClockAnchorGeneration = true;
    }
    return decision;
}

inline bool MonitorWorldIsCurrent(WorldGeneration currentWorld,
                                  WorldGeneration recordWorld) noexcept {
    if (currentWorld == kNoWorldGeneration ||
        recordWorld == kNoWorldGeneration) {
        return true;
    }
    return currentWorld == recordWorld;
}

} // namespace harken

#endif
