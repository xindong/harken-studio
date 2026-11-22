#pragma once

#include "PlaybackBridge.h"
#include <harken_studio/AudioValues.h>

namespace harken::studio::bridge {

inline constexpr int32_t kPreviewOk = 0;
inline constexpr int32_t kPreviewInvalidArg = 1;
inline constexpr int32_t kPreviewUnknownHandle = 4;
inline constexpr int32_t kPreviewStaleHandle = 5;
inline constexpr int32_t kPreviewHostError = 11;
inline constexpr int32_t kPreviewBackpressure = 16;
inline constexpr uint32_t kPreviewPlayableSound = 0;
inline constexpr uint32_t kPreviewPlayableContainer = 1;
inline constexpr uint32_t kPreviewPlayableTrack = 2;
inline constexpr uint32_t kPreviewPlayableSegment = 3;
inline constexpr uint32_t kPreviewLifecycleActive = 2;
inline constexpr uint32_t kPreviewLifecycleLogicalComplete = 4;
inline constexpr EngineResult kEngineNoVariable = static_cast<EngineResult>(-306);
struct PreviewPlayableState {
    uint32_t playableKind = 0;
    uint32_t lifecycle = 0;
    uint32_t reason = 0;
    harken::studio::types::ObjectId stableId{};
    uint64_t worldGeneration = 0;
    uint64_t acceptedLogicTicket = 0;
    uint64_t lastAppliedBlockSequence = 0;
    uint32_t childLiveCount = 0;
    uint32_t childAudibleRetiringCount = 0;
    uint32_t pendingFutureCount = 0;
};

struct VariableTicket {
    uint64_t sequence = 0;
    uint64_t worldGeneration = 0;
    uint64_t transportGeneration = 0;
};

int32_t playPreviewPlayable(uint32_t kind, harken::studio::types::ObjectId id,
                            uint64_t emitter, uint64_t& handle);
int32_t resetPreviewPlayable(uint32_t kind, harken::studio::types::ObjectId id, uint64_t emitter);
int32_t stopPreviewPlayable(uint64_t handle, uint32_t fadeMs);
int32_t queryPreviewPlayable(uint64_t handle, PreviewPlayableState& state);
uint64_t createPreviewEmitter(EngineResult* outResult = nullptr);
void destroyPreviewEmitter(uint64_t emitter);
enum class VariableWriteState { Pending, Applied, Unavailable };
VariableWriteState queryVariableWriteState(const VariableTicket& ticket);
EngineResult writePreviewVariable(harken::studio::types::ObjectId id, float value,
                                  uint64_t emitter, VariableTicket& ticket);
struct PreviewBindingDefinition
{
    struct Point { float x = 0; float y = 0; };
    struct Handles { float h1x = 0; float h1y = 0; float h2x = 0; float h2y = 0; };
    harken::studio::types::ObjectId variableId{};
    uint32_t targetScopeKind = 0;
    uint32_t targetId = 0;
    uint32_t transitionMs = 0;
    uint32_t pointCount = 0;
    Point points[harken::studio::types::kCurveMaxPoints]{};
    Handles handles[harken::studio::types::kCurveMaxPoints - 1]{};
};

EngineResult setPreviewBinding(harken::studio::types::ObjectId id,
                               const PreviewBindingDefinition& definition);
EngineResult attachPreviewBinding(harken::studio::types::ObjectId id, uint32_t scope, harken::studio::types::ObjectId object);
void detachPreviewBinding(harken::studio::types::ObjectId id, uint32_t scope, harken::studio::types::ObjectId object);
void removePreviewBinding(harken::studio::types::ObjectId id);
struct PreviewVoiceEffectEntry {
    harken::studio::types::ObjectId effectId{};
    uint32_t effectType = 0;
    bool bypass = false;
    float ceilingDb = 0.0f;
    float releaseMs = 100.0f;
    bool operator==(const PreviewVoiceEffectEntry&) const = default;
};
EngineResult setPreviewVoiceEffectChain(uint64_t voice,
    const std::vector<PreviewVoiceEffectEntry>& chain);
EngineResult refreshPreviewVoiceBindings(uint64_t voice);
EngineResult setPreviewVoiceProperty(uint64_t voice, uint8_t property, float value,
                                     harken::studio::types::ObjectId sendBus = {});
EngineResult setPreviewVoiceEffect(uint64_t voice, harken::studio::types::ObjectId effectId,
    uint32_t effectType, const std::vector<float>& parameters, bool bypass);
bool currentWorldBusAdmissionIsOpen(uint64_t generation);
EngineResult setCurrentWorldBusMute(harken::studio::types::ObjectId bus, bool mute, uint32_t fadeMs);
EngineResult setCurrentWorldBusSolo(harken::studio::types::ObjectId bus, bool solo, uint32_t fadeMs);
EngineResult setCurrentWorldBusOutputVolume(harken::studio::types::ObjectId bus, float db, uint32_t fadeMs);
uint64_t registeredSoundRevision(harken::studio::types::ObjectId sound);

}
