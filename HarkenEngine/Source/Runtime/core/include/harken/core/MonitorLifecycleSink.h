#ifndef HARKEN_CORE_MONITOR_LIFECYCLE_SINK_H
#define HARKEN_CORE_MONITOR_LIFECYCLE_SINK_H

#include <harken/ObjectId.h>
#include "harken/core/MonitorRequest.h"
#include "harken/core/MonitorEmitterIdentity.h"
#include "harken/core/MonitorIdentity.h"
#include <cstdint>

namespace harken {

// Projection phases for already-decided Runtime facts. Values are local to
// this sink; public Monitor event classes are chosen by the Hub mapper.
enum class MonitorLifecyclePhase : std::uint8_t {
    PlayableStart = 1,
    PlayableLogicalComplete = 2,
    PlayableAudibleRetiring = 3,
    PlayableRetired = 4,
    PlayableCancel = 5,
    ActionAccept = 6,
    ActionStop = 7,
    ActionResult = 8,
    ActionReject = 9,
    QueueAdvance = 10,
    QueueFail = 11,
    TransitionStart = 12,
    TransitionStop = 13,
    TransitionReject = 14,
    VoicePending = 15,
    VoiceLive = 16,
    VoicePark = 17,
    VoiceEnd = 18,
    EffectInstalled = 19,
    EffectReleased = 20,
    SelectionCommitted = 21,
    PreparedCandidate = 22
};

// Called after the reported Runtime decision succeeds. No-op when Monitor is absent,
// the Hub is down, or the caller is not the logic thread. Never writes
// Runtime tables and never invents a VariableControl entity.
//
// Facts already present on the committing table row. Missing values stay 0
// and must not be invented by the Hub mapper.
struct MonitorLifecycleFacts {
    MonitorRequestContext playbackRequest{};
    MonitorEmitterOrigin emitterOrigin{};
    MonitorControlCause controlCause{};
    MonitorControlCause selectionCause{};
    bool controlObservationIncomplete = false;
    bool identityObservationIncomplete = false;
    bool publishLegacy = true;
    bool reasonKnown = true;
    MonitorOccurrenceIdentity occurrenceIdentity{};
    VnObjectId contentObject{};
    std::uint64_t contentWorld = 0;
    std::uint8_t contentKind = 0;
    std::uint64_t contentRevision = 0;
    std::uint64_t parentPlayable = 0;
    std::uint64_t parentAction = 0;
    std::uint64_t root = 0;
    std::uint64_t relatedAction = 0;
    std::uint64_t serviceLeaf = 0;
    std::uint64_t resourceOwner = 0;
    std::uint64_t ownerEpoch = 0;
    std::uint64_t logicalVoice = 0;
    std::uint64_t renderActivation = 0;
    VnObjectId projectId{};
    std::uint64_t programSemanticFingerprint = 0;
    std::uint64_t runtimeLineageFingerprint = 0;
    std::uint64_t arrangingInstance = 0;
    std::uint64_t logicCommitSequence = 0;
    float volumeDb = 0.0f;
    bool volumeKnown = false;
    std::uint32_t notifyType = 0;
};

void ObserveMonitorLifecycle(MonitorLifecyclePhase phase,
                             std::uint64_t instanceHandle,
                             std::uint64_t instanceGeneration,
                             std::uint64_t worldGeneration,
                             VnObjectId stableId,
                             std::uint64_t relatedPlayableHandle,
                             std::uint64_t relatedActionHandle,
                             std::uint32_t reason,
                             std::uint8_t playableKind,
                             std::uint64_t occurrence = 0,
                             const MonitorLifecycleFacts& facts = {}) noexcept;

// Called after a successful non-idempotent CurrentWorld install. Reads the
// already-committed generation; does not drive world replacement.
void ObserveMonitorCurrentWorld(std::uint64_t worldGeneration) noexcept;

struct MonitorEffectFacts {
    std::uint32_t bypass01 = 0;
    std::uint32_t latencyFrames = 0;
    std::uint32_t tailFrames = 0;
    bool tailKnown = false;
};

void ObserveMonitorEffect(std::uint64_t instanceHandle,
                          std::uint64_t instanceGeneration,
                          VnObjectId stableId, std::uint32_t reason,
                          const MonitorEffectFacts& facts) noexcept;

struct MonitorBusFacts {
    std::uint32_t mute01 = 0;
    std::uint32_t solo01 = 0;
    VnObjectId routeBusId{};
};

void ObserveMonitorBus(std::uint64_t instanceHandle,
                       std::uint64_t instanceGeneration, VnObjectId stableId,
                       const MonitorBusFacts& facts) noexcept;

void ObserveMonitorEffectRelease(std::uint64_t instanceHandle,
                                 std::uint64_t instanceGeneration,
                                 VnObjectId stableId,
                                 std::uint32_t reason) noexcept;

void ObserveMonitorDspGap() noexcept;
void ObserveMonitorSelectionUnavailable() noexcept;

} // namespace harken

#endif
