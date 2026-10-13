#ifndef HARKEN_API_PLAYABLE_RUNTIME_H
#define HARKEN_API_PLAYABLE_RUNTIME_H

#include <harken/ObjectId.h>
#include "harken/core/CurrentWorld.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/PlayablePropertyCut.h"
#include "harken/core/TimelineProgram.h"

#include <cstdint>

namespace harken {

struct Engine;

struct PlaybackPathObservation {
    std::uint64_t voiceInstance = 0;
    VnObjectId soundId{};
    VnObjectId outputBusId{};
    PlaybackPath path{};
    std::uint32_t sendCount = 0;
    TimelineVoiceSendInstall sends[kTimelineMaxVoiceSends]{};
};

bool ObserveCurrentPlaybackPaths(
    void (*visitor)(const PlaybackPathObservation&, void*), void* context) noexcept;

VnResult SetPreviewVoiceProperty(std::uint64_t voiceInstance,
    playableproperties::PlayablePropertyIdV1 property, float value,
    VnObjectId sendBusId = {}) noexcept;
VnResult RefreshPreviewVoiceBindings(std::uint64_t voiceInstance) noexcept;

enum class PlayableStartStatus : std::uint8_t {
    Accepted = 0,
    InvalidInput = 1,
    UnknownPlayable = 2,
    MediaMismatch = 3,
    Capacity = 4,
    Backpressure = 5,
    CapabilityUnavailable = 6,
    RuntimeRecreateRequired = 7,
    NotInitialized = 8,
    GenerationConflict = 9,
};

enum class BusAdmissionVerdict : std::uint8_t {
    Admit = 0,
    Backpressure = 1,
    GenerationConflict = 2,
    NotAuthored = 3,
};

struct BusAdmissionClaimKeyV1 {
    std::uint32_t tag = 0;
    std::uint32_t zero = 0;
    std::uint64_t fields[5]{};
};

enum class BusAdmissionClaimState : std::uint8_t {
    Closed = 0,
    Pending = 1,
    Open = 2,
};

inline constexpr std::uint32_t kBusAdmissionTagStudioCurrentWorld = 1;
inline constexpr std::uint32_t kBusAdmissionTagPackActivation = 2;

BusAdmissionVerdict CheckPlayableWorldBusAdmissionV1(
    const Engine& engine, const PlayableWorldViewV1& world) noexcept;
bool ArmCurrentWorldBusAdmissionV1(
    Engine& engine, WorldGeneration generation,
    const BusAdmissionClaimKeyV1& key,
    BusAdmissionClaimState state) noexcept;
void RecycleStaleCurrentWorldBusAdmissionV1(
    Engine& engine, WorldGeneration keepGeneration) noexcept;
void ClearCurrentWorldBusAdmissionV1(Engine& engine) noexcept;

struct PlayableStartRequest {
    PlayableRefV1 root{};
    std::uint64_t callerSeed = 0;
    std::uint64_t emitter = 0;
};

struct PlayableStartResult {
    PlayableStartStatus status = PlayableStartStatus::InvalidInput;
    PlayableInstanceHandle instance = 0;
    std::uint64_t ticket = 0;
};

enum class PlayableControlLifecycle : std::uint8_t {
    Preparing = 1,
    Active = 2,
    Cancelling = 3,
    LogicalComplete = 4,
    AudibleRetiring = 5,
    FailingSourceActive = 6,
    FailingAudibleRetiring = 7,
};

enum class PlayableControlReason : std::uint8_t {
    None = 0,
    Natural = 1,
    StopRequested = 2,
    MediaMissing = 3,
    ContentRevision = 4,
    StartRefused = 5,
    Deadline = 6,
};

enum class PlayableControlStatus : std::uint8_t {
    Ok = 0,
    InvalidHandle = 1,
    StaleHandle = 2,
    Backpressure = 3,
    NotInitialized = 4,
};

enum class PlayableResetStatus : std::uint8_t {
    Ok = 0,
    InvalidInput = 1,
    UnknownPlayable = 2,
    Backpressure = 3,
    CapabilityUnavailable = 4,
    RuntimeRecreateRequired = 5,
    NotInitialized = 6,
};

struct PlayableControlState {
    PlayableKindV1 kind = PlayableKindV1::Sound;
    PlayableControlLifecycle lifecycle =
        PlayableControlLifecycle::Preparing;
    PlayableControlReason reason = PlayableControlReason::None;
    VnObjectId stableId{};
    std::uint64_t worldGeneration = 0;
    std::uint64_t acceptedLogicTicket = 0;
    std::uint64_t lastAppliedBlockSequence = 0;
    std::uint32_t childLiveCount = 0;
    std::uint32_t childAudibleRetiringCount = 0;
    std::uint32_t pendingFutureCount = 0;
};

enum class RuntimeTransportResetStatus : std::uint8_t {
    Complete = 0,
    Backpressure = 1,
    InvalidRequest = 2,
    RuntimeRecreateRequired = 3,
};

PlayableStartResult StartPlayable(Engine& engine,
                                  const PlayableStartRequest& request) noexcept;
PlayableStartResult StartPlayable(
    Engine& engine, const PlayableWorldViewV1& pinnedWorld,
    const PlayableStartRequest& request) noexcept;
PlayableControlStatus StopPlayable(
    Engine& engine, PlayableInstanceHandle instance,
    std::uint32_t fadeMs) noexcept;
PlayableControlStatus QueryPlayable(
    const Engine& engine, PlayableInstanceHandle instance,
    PlayableControlState* out) noexcept;
bool HasCurrentWorldPlayableConsumers(const Engine& engine) noexcept;
bool HasCurrentWorldPublicationConsumers(const Engine& engine) noexcept;
void ReclaimRetiredSelectionMemory(Engine& engine) noexcept;
void AdvanceCurrentWorldReplacementDrain(Engine& engine) noexcept;
bool CancelActionDeferredForWorldReplacement(
    Engine& engine, std::uint64_t predecessorGeneration) noexcept;
RuntimeTransportResetStatus BeginRuntimeTransportReset(
    Engine& engine, std::uint64_t resetFrame) noexcept;

PlayableStartResult StartCurrentPlayable(
    const PlayableStartRequest& request) noexcept;
PlayableStartResult StartCurrentPlayableByIdentity(
    PlayableKindV1 kind, VnObjectId stableId,
    std::uint64_t emitter) noexcept;
PlayableResetStatus ResetCurrentPlayableByIdentity(
    PlayableKindV1 kind, VnObjectId stableId,
    std::uint64_t emitter) noexcept;
PlayableControlStatus StopCurrentPlayable(
    PlayableInstanceHandle instance, std::uint32_t fadeMs) noexcept;
PlayableControlStatus QueryCurrentPlayable(
    PlayableInstanceHandle instance, PlayableControlState* out) noexcept;

void RecordPreviewPlayPlayable(
    bool argsReadable, std::uint32_t structSize, std::uint32_t kind,
    VnObjectId stableId, std::uint64_t emitter, std::uint32_t flags,
    std::uint32_t reserved, std::int32_t result,
    std::uint32_t returnedKind, std::uint64_t handle) noexcept;
void RecordPreviewResetPlayable(
    bool argsReadable, std::uint32_t structSize, std::uint32_t kind,
    VnObjectId stableId, std::uint64_t emitter, std::uint32_t flags,
    std::uint32_t reserved, std::int32_t result) noexcept;
void RecordPreviewStopPlayable(std::uint64_t handle, std::uint32_t fadeMs,
                               std::int32_t result) noexcept;
void RecordPreviewQueryPlayable(
    std::uint64_t handle, bool stateReadable, std::uint32_t structSize,
    std::int32_t result, const PlayableControlState* state) noexcept;

}

#endif
