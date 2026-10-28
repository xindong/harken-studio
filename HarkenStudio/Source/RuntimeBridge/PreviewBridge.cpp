#include "PreviewBridge.h"
#include "NativeValues.h"
#include "RuntimeLink.h"

#include <harken/Harken.h>
#include <harken_preview/PreviewAbi.h>

namespace HarkenEditor::Bridge {

static_assert(kEngineNoVariable == static_cast<EngineResult>(VN_NO_VARIABLE));
static_assert(kEngineStaleHandle == static_cast<EngineResult>(VN_STALE_HANDLE));
static_assert(kPreviewPlayableSegment == VN_PREVIEW_PLAYABLE_KIND_SEGMENT);
static_assert(kPreviewBackpressure == VN_PREVIEW_BACKPRESSURE);

int32_t playPreviewPlayable(uint32_t kind, harkenstudio::ObjectId id,
                            uint64_t emitter, uint64_t& handle)
{
    VnPreviewPlayPlayableDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.playableKind = kind;
    desc.stableId = toNative(id);
    desc.emitterHandle = emitter;
    return VnPreview_PlayPlayable(&desc, &handle);
}

int32_t resetPreviewPlayable(uint32_t kind, harkenstudio::ObjectId id, uint64_t emitter)
{
    VnPreviewResetPlayableDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.playableKind = kind;
    desc.stableId = toNative(id);
    desc.emitterHandle = emitter;
    return VnPreview_ResetPlayable(&desc);
}

int32_t stopPreviewPlayable(uint64_t handle, uint32_t fadeMs)
{
    return VnPreview_StopPlayable(handle, fadeMs);
}

int32_t queryPreviewPlayable(uint64_t handle, PreviewPlayableState& out)
{
    VnPreviewPlayableStateV2 state{};
    state.structSize = sizeof(state);
    const auto result = VnPreview_QueryPlayable(handle, &state);
    if (result == VN_PREVIEW_OK) {
        out = {state.playableKind, state.lifecycle, state.reason, fromNative(state.stableId),
               state.worldGeneration, state.acceptedLogicTicket, state.lastAppliedBlockSequence,
               state.childLiveCount, state.childAudibleRetiringCount, state.pendingFutureCount};
    }
    return result;
}

uint64_t createPreviewEmitter(EngineResult* outResult)
{
    return Detail::runtimeCreatePreviewEmitter(outResult);
}

void destroyPreviewEmitter(uint64_t emitter) { Vn_DestroyEmitter(emitter); }

VariableWriteState queryVariableWriteState(const VariableTicket& ticket)
{
    VnVariableTicketQueryDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.ticket = {ticket.sequence, ticket.worldGeneration, ticket.transportGeneration};
    VnVariableTicketQueryResultV1 result{};
    result.structSize = sizeof(result);
    if (Vn_QueryVariableTicket(&desc, &result) != VN_OK
        || result.status != VN_VARIABLE_TICKET_FOUND)
        return VariableWriteState::Unavailable;
    if (result.outcome == VN_VARIABLE_OUTCOME_PENDING)
        return VariableWriteState::Pending;
    return result.outcome == VN_VARIABLE_OUTCOME_APPLIED
        ? VariableWriteState::Applied : VariableWriteState::Unavailable;
}

EngineResult writePreviewVariable(harkenstudio::ObjectId id, float value,
                                  uint64_t emitter, VariableTicket& ticket)
{
    VnVariableWriteV1 write{};
    write.variableId = toNative(id);
    write.value = value;
    VnVariableBatchDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.writeCount = 1;
    desc.writes = &write;
    VnVariableTicketV1 nativeTicket{};
    const auto result = emitter == 0 ? Vn_SetValues(&desc, &nativeTicket)
        : Vn_SetEmitterValues(emitter, &desc, &nativeTicket);
    if (result == VN_OK)
        ticket = {nativeTicket.sequence, nativeTicket.worldGeneration, nativeTicket.transportGeneration};
    return static_cast<EngineResult>(result);
}

EngineResult setPreviewBinding(harkenstudio::ObjectId id,
                               const harkenstudiobuild::BindingDefinition& definition)
{
    if (definition.curve.pointCount > VN_CURVE_MAX_POINTS)
        return static_cast<EngineResult>(VN_INVALID_ARG);
    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = toNative(definition.variableId);
    desc.targetScopeKind = definition.targetScopeKind;
    desc.targetId = definition.targetId;
    desc.transitionMs = definition.transitionMs;
    desc.curve.structSize = sizeof(desc.curve);
    desc.curve.pointCount = definition.curve.pointCount;
    for (uint32_t i = 0; i < desc.curve.pointCount; ++i) {
        desc.curve.points[i] = {definition.curve.points[i].x, definition.curve.points[i].y};
        if (i + 1 < desc.curve.pointCount) {
            const auto& h = definition.curve.handles[i];
            desc.curve.handles[i] = {h.h1x, h.h1y, h.h2x, h.h2y};
        }
    }
    const auto nativeId = toNative(id);
    auto result = Vn_RegisterBinding(nativeId, &desc);
    if (result == VN_IN_USE) result = Vn_ReplaceBinding(nativeId, &desc);
    return static_cast<EngineResult>(result);
}

EngineResult attachPreviewBinding(harkenstudio::ObjectId id, uint32_t scope, harkenstudio::ObjectId object)
{
    VnBindingAttachDesc desc{sizeof(VnBindingAttachDesc), scope, toNative(object)};
    return static_cast<EngineResult>(Vn_AttachBinding(toNative(id), &desc));
}

void detachPreviewBinding(harkenstudio::ObjectId id, uint32_t scope, harkenstudio::ObjectId object)
{
    VnBindingAttachDesc desc{sizeof(VnBindingAttachDesc), scope, toNative(object)};
    Vn_DetachBinding(toNative(id), &desc);
}

void removePreviewBinding(harkenstudio::ObjectId id) { Vn_UnregisterBinding(toNative(id)); }

EngineResult refreshPreviewVoiceBindings(uint64_t voice)
{
    return Detail::runtimeRefreshPreviewVoiceBindings(voice);
}

EngineResult setPreviewVoiceProperty(uint64_t voice, uint8_t property, float value,
                                     harkenstudio::ObjectId sendBus)
{
    return Detail::runtimeSetPreviewVoiceProperty(voice, property, value, sendBus);
}

bool currentWorldBusAdmissionIsOpen(uint64_t generation)
{
    return Detail::runtimeCurrentWorldBusAdmissionIsOpen(generation);
}

EngineResult setCurrentWorldBusMute(harkenstudio::ObjectId bus, bool mute, uint32_t fadeMs)
{
    return Detail::runtimeSetBusMute(bus, mute, fadeMs);
}

EngineResult setCurrentWorldBusSolo(harkenstudio::ObjectId bus, bool solo, uint32_t fadeMs)
{
    return Detail::runtimeSetBusSolo(bus, solo, fadeMs);
}

EngineResult setCurrentWorldBusOutputVolume(harkenstudio::ObjectId bus, float db, uint32_t fadeMs)
{
    return Detail::runtimeSetBusOutputVolume(bus, db, fadeMs);
}

uint64_t registeredSoundRevision(harkenstudio::ObjectId sound)
{
    return Detail::runtimeRegisteredSoundRevision(sound);
}

}
