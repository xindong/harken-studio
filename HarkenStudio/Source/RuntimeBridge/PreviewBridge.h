#pragma once

#include "PlaybackBridge.h"
#include <harken_studio_build/AuthoredParameters.h>

namespace HarkenEditor::Bridge {

inline constexpr int32_t kPreviewOk = 0;
inline constexpr int32_t kPreviewInvalidArg = 1;
inline constexpr int32_t kPreviewUnknownHandle = 4;
inline constexpr int32_t kPreviewStaleHandle = 5;
inline constexpr int32_t kPreviewHostError = 11;
inline constexpr int32_t kPreviewBackpressure = 16;
inline constexpr uint32_t kPreviewPlayableSound = 0;
inline constexpr uint32_t kPreviewPlayableContainer = 1;
inline constexpr uint32_t kPreviewPlayableTrack = 2;
inline constexpr uint32_t kPreviewPlayablePiece = 3;
inline constexpr uint32_t kPreviewLifecycleActive = 2;
inline constexpr uint32_t kPreviewLifecycleLogicalComplete = 4;
inline constexpr EngineResult kEngineNoVariable = static_cast<EngineResult>(-306);
struct PreviewPlayableState {
    uint32_t playableKind = 0;
    uint32_t lifecycle = 0;
    uint32_t reason = 0;
    harkenstudio::ObjectId stableId{};
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

int32_t playPreviewPlayable(uint32_t kind, harkenstudio::ObjectId id,
                            uint64_t emitter, uint64_t& handle);
int32_t resetPreviewPlayable(uint32_t kind, harkenstudio::ObjectId id, uint64_t emitter);
int32_t stopPreviewPlayable(uint64_t handle, uint32_t fadeMs);
int32_t queryPreviewPlayable(uint64_t handle, PreviewPlayableState& state);
uint64_t createPreviewEmitter(EngineResult* outResult = nullptr);
void destroyPreviewEmitter(uint64_t emitter);
enum class VariableWriteState { Pending, Applied, Unavailable };
VariableWriteState queryVariableWriteState(const VariableTicket& ticket);
EngineResult writePreviewVariable(harkenstudio::ObjectId id, float value,
                                  uint64_t emitter, VariableTicket& ticket);
EngineResult setPreviewBinding(harkenstudio::ObjectId id,
                               const harkenstudiobuild::BindingDefinition& definition);
EngineResult attachPreviewBinding(harkenstudio::ObjectId id, uint32_t scope, harkenstudio::ObjectId object);
void detachPreviewBinding(harkenstudio::ObjectId id, uint32_t scope, harkenstudio::ObjectId object);
void removePreviewBinding(harkenstudio::ObjectId id);
EngineResult refreshPreviewVoiceBindings(uint64_t voice);
EngineResult setPreviewVoiceProperty(uint64_t voice, uint8_t property, float value,
                                     harkenstudio::ObjectId sendBus = {});
bool currentWorldBusAdmissionIsOpen(uint64_t generation);
EngineResult setCurrentWorldBusMute(harkenstudio::ObjectId bus, bool mute, uint32_t fadeMs);
EngineResult setCurrentWorldBusSolo(harkenstudio::ObjectId bus, bool solo, uint32_t fadeMs);
EngineResult setCurrentWorldBusOutputVolume(harkenstudio::ObjectId bus, float db, uint32_t fadeMs);
uint64_t registeredSoundRevision(harkenstudio::ObjectId sound);

}
