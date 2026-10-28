#include "harken_preview/PreviewAbi.h"

#include "HarkenApiInternal.h"

#include <cstdint>

#if !defined(HARKEN_DEVELOPMENT_PREVIEW)
#error "HARKEN_DEVELOPMENT_PREVIEW must be defined to 1 or 0"
#endif

namespace {

constexpr bool kGateOpen = HARKEN_DEVELOPMENT_PREVIEW != 0;
constexpr std::uint64_t kPreviewHandleTag = std::uint64_t{1} << 63;

bool ValidKind(std::uint32_t kind) noexcept {
    return kind <= VN_PREVIEW_PLAYABLE_KIND_SEGMENT;
}

harken::PlayableKindV1 ToKind(std::uint32_t kind) noexcept {
    return static_cast<harken::PlayableKindV1>(kind);
}

VnPreviewPlayableHandle ToPreviewHandle(
    harken::PlayableInstanceHandle handle) noexcept {
    return handle | kPreviewHandleTag;
}

bool ToInternalHandle(VnPreviewPlayableHandle handle,
                      harken::PlayableInstanceHandle* out) noexcept {
    if (out == nullptr || (handle & kPreviewHandleTag) == 0) {
        return false;
    }
    const std::uint64_t internal = handle & ~kPreviewHandleTag;
    if (internal == 0) {
        return false;
    }
    *out = internal;
    return true;
}

VnPreviewResult MapStart(harken::PlayableStartStatus status) noexcept {
    switch (status) {
        case harken::PlayableStartStatus::Accepted:
            return VN_PREVIEW_OK;
        case harken::PlayableStartStatus::InvalidInput:
            return VN_PREVIEW_INVALID_ARG;
        case harken::PlayableStartStatus::UnknownPlayable:
        case harken::PlayableStartStatus::MediaMismatch:
            return VN_PREVIEW_UNKNOWN_ID;
        case harken::PlayableStartStatus::Capacity:
            return VN_PREVIEW_NO_VOICE;
        case harken::PlayableStartStatus::Backpressure:
            return VN_PREVIEW_BACKPRESSURE;
        case harken::PlayableStartStatus::CapabilityUnavailable:
            return VN_PREVIEW_NOT_SUPPORTED;
        case harken::PlayableStartStatus::RuntimeRecreateRequired:
        case harken::PlayableStartStatus::NotInitialized:
            return VN_PREVIEW_HOST_ERROR;
        case harken::PlayableStartStatus::GenerationConflict:
            return VN_PREVIEW_REVISION_CONFLICT;
    }
    return VN_PREVIEW_HOST_ERROR;
}

VnPreviewResult MapControl(harken::PlayableControlStatus status) noexcept {
    switch (status) {
        case harken::PlayableControlStatus::Ok:
            return VN_PREVIEW_OK;
        case harken::PlayableControlStatus::InvalidHandle:
            return VN_PREVIEW_UNKNOWN_HANDLE;
        case harken::PlayableControlStatus::StaleHandle:
            return VN_PREVIEW_STALE_HANDLE;
        case harken::PlayableControlStatus::Backpressure:
            return VN_PREVIEW_BACKPRESSURE;
        case harken::PlayableControlStatus::NotInitialized:
            return VN_PREVIEW_HOST_ERROR;
    }
    return VN_PREVIEW_HOST_ERROR;
}

VnPreviewResult MapReset(harken::PlayableResetStatus status) noexcept {
    switch (status) {
        case harken::PlayableResetStatus::Ok:
            return VN_PREVIEW_OK;
        case harken::PlayableResetStatus::InvalidInput:
            return VN_PREVIEW_INVALID_ARG;
        case harken::PlayableResetStatus::UnknownPlayable:
            return VN_PREVIEW_UNKNOWN_ID;
        case harken::PlayableResetStatus::Backpressure:
            return VN_PREVIEW_BACKPRESSURE;
        case harken::PlayableResetStatus::CapabilityUnavailable:
            return VN_PREVIEW_NOT_SUPPORTED;
        case harken::PlayableResetStatus::RuntimeRecreateRequired:
        case harken::PlayableResetStatus::NotInitialized:
            return VN_PREVIEW_HOST_ERROR;
    }
    return VN_PREVIEW_HOST_ERROR;
}

}

extern "C" {

VnPreviewResult VnPreview_CapturePlaybackPaths(
    VnPreviewPlaybackPathV1* outPaths, uint32_t capacity, uint32_t* requiredCount) {
    if (requiredCount != nullptr) *requiredCount = 0;
    if (!kGateOpen) return VN_PREVIEW_DEVELOPMENT_API_DISABLED;
    if (requiredCount == nullptr || (capacity != 0 && outPaths == nullptr))
        return VN_PREVIEW_INVALID_ARG;
    static_assert(VN_PREVIEW_PATH_ROW_CAPACITY == harken::kPlaybackPathRowCapacity);
    static_assert(VN_PREVIEW_PATH_SEND_CAPACITY == harken::kTimelineMaxVoiceSends);
    struct Capture {
        VnPreviewPlaybackPathV1* paths;
        uint32_t capacity;
        uint32_t count;
    } capture{outPaths, capacity, 0};
    const bool available = harken::ObserveCurrentPlaybackPaths(
        [](const harken::PlaybackPathObservation& source, void* context) {
            auto& result = *static_cast<Capture*>(context);
            const uint32_t index = result.count++;
            if (index >= result.capacity) return;
            auto& path = result.paths[index];
            path = {};
            path.structSize = sizeof(path);
            path.voiceInstance = source.voiceInstance;
            path.soundId = source.soundId;
            path.outputBusId = source.outputBusId;
            path.rowCount = source.path.rowCount;
            path.sendCount = source.sendCount;
            for (uint32_t row = 0; row < path.rowCount; ++row) {
                path.rows[row].stableId = source.path.rows[row].stableId;
                path.rows[row].occurrenceOrdinal = source.path.rows[row].occurrenceOrdinal;
            }
            for (uint32_t send = 0; send < path.sendCount; ++send) {
                path.sends[send].targetBusId = source.sends[send].targetBusId;
                path.sends[send].levelDb = source.sends[send].levelDb;
            }
        }, &capture);
    if (!available) return VN_PREVIEW_HOST_ERROR;
    *requiredCount = capture.count;
    return capture.count > capacity ? VN_PREVIEW_RESULT_CAPACITY : VN_PREVIEW_OK;
}

VnPreviewResult VnPreview_PlayPlayable(
    const VnPreviewPlayPlayableDescV2* desc,
    VnPreviewPlayableHandle* outHandle) {
    if (outHandle != nullptr) {
        *outHandle = 0;
    }
    if (!kGateOpen) {
        harken::RecordPreviewPlayPlayable(
            false, 0, 0, {}, 0, 0, 0,
            VN_PREVIEW_DEVELOPMENT_API_DISABLED, 0, 0);
        return VN_PREVIEW_DEVELOPMENT_API_DISABLED;
    }
    if (desc == nullptr || outHandle == nullptr) {
        harken::RecordPreviewPlayPlayable(
            false, desc == nullptr ? 0 : desc->structSize, 0, {}, 0, 0, 0,
            VN_PREVIEW_INVALID_ARG, 0, 0);
        return VN_PREVIEW_INVALID_ARG;
    }
    if (desc->structSize != sizeof(VnPreviewPlayPlayableDescV2)) {
        harken::RecordPreviewPlayPlayable(
            false, desc->structSize, 0, {}, 0, 0, 0,
            VN_PREVIEW_STRUCT_SIZE_MISMATCH, 0, 0);
        return VN_PREVIEW_STRUCT_SIZE_MISMATCH;
    }
    if (!ValidKind(desc->playableKind) || desc->stableId == VnObjectId{} ||
        desc->flags != 0 || desc->reserved0 != 0) {
        harken::RecordPreviewPlayPlayable(
            true, desc->structSize, desc->playableKind, desc->stableId,
            desc->emitterHandle, desc->flags, desc->reserved0,
            VN_PREVIEW_INVALID_ARG, 0, 0);
        return VN_PREVIEW_INVALID_ARG;
    }
    try {
        const auto request = harken::detail::BeginMonitorRequest(
            harken::MonitorRequestOrigin::StudioPreview,
            VN_MONITOR_OPERATION_PLAY_PREVIEW);
        const harken::PlayableStartResult result =
            harken::StartCurrentPlayableByIdentity(
                ToKind(desc->playableKind), desc->stableId,
                desc->emitterHandle, request);
        const VnPreviewResult mapped = MapStart(result.status);
        if (mapped == VN_PREVIEW_OK) {
            *outHandle = ToPreviewHandle(result.instance);
        }
        harken::RecordPreviewPlayPlayable(
            true, desc->structSize, desc->playableKind, desc->stableId,
            desc->emitterHandle, desc->flags, desc->reserved0, mapped,
            mapped == VN_PREVIEW_OK ? desc->playableKind : 0,
            mapped == VN_PREVIEW_OK ? *outHandle : 0);
        return mapped;
    } catch (...) {
        harken::RecordPreviewPlayPlayable(
            true, desc->structSize, desc->playableKind, desc->stableId,
            desc->emitterHandle, desc->flags, desc->reserved0,
            VN_PREVIEW_HOST_ERROR, 0, 0);
        return VN_PREVIEW_HOST_ERROR;
    }
}

VnPreviewResult VnPreview_ResetPlayable(
    const VnPreviewResetPlayableDescV2* desc) {
    if (!kGateOpen) {
        harken::RecordPreviewResetPlayable(
            false, 0, 0, {}, 0, 0, 0,
            VN_PREVIEW_DEVELOPMENT_API_DISABLED);
        return VN_PREVIEW_DEVELOPMENT_API_DISABLED;
    }
    if (desc == nullptr) {
        harken::RecordPreviewResetPlayable(
            false, 0, 0, {}, 0, 0, 0, VN_PREVIEW_INVALID_ARG);
        return VN_PREVIEW_INVALID_ARG;
    }
    if (desc->structSize != sizeof(VnPreviewResetPlayableDescV2)) {
        harken::RecordPreviewResetPlayable(
            false, desc->structSize, 0, {}, 0, 0, 0,
            VN_PREVIEW_STRUCT_SIZE_MISMATCH);
        return VN_PREVIEW_STRUCT_SIZE_MISMATCH;
    }
    if (!ValidKind(desc->playableKind) || desc->stableId == VnObjectId{} ||
        desc->flags != 0 || desc->reserved0 != 0) {
        harken::RecordPreviewResetPlayable(
            true, desc->structSize, desc->playableKind, desc->stableId,
            desc->emitterHandle, desc->flags, desc->reserved0,
            VN_PREVIEW_INVALID_ARG);
        return VN_PREVIEW_INVALID_ARG;
    }
    try {
        const VnPreviewResult mapped = MapReset(
            harken::ResetCurrentPlayableByIdentity(
                ToKind(desc->playableKind), desc->stableId,
                desc->emitterHandle));
        harken::RecordPreviewResetPlayable(
            true, desc->structSize, desc->playableKind, desc->stableId,
            desc->emitterHandle, desc->flags, desc->reserved0, mapped);
        return mapped;
    } catch (...) {
        harken::RecordPreviewResetPlayable(
            true, desc->structSize, desc->playableKind, desc->stableId,
            desc->emitterHandle, desc->flags, desc->reserved0,
            VN_PREVIEW_HOST_ERROR);
        return VN_PREVIEW_HOST_ERROR;
    }
}

VnPreviewResult VnPreview_StopPlayable(VnPreviewPlayableHandle handle,
                                       uint32_t fadeMs) {
    if (!kGateOpen) {
        harken::RecordPreviewStopPlayable(
            handle, fadeMs, VN_PREVIEW_DEVELOPMENT_API_DISABLED);
        return VN_PREVIEW_DEVELOPMENT_API_DISABLED;
    }
    harken::PlayableInstanceHandle internal = 0;
    if (!ToInternalHandle(handle, &internal)) {
        harken::RecordPreviewStopPlayable(
            handle, fadeMs, VN_PREVIEW_INVALID_ARG);
        return VN_PREVIEW_INVALID_ARG;
    }
    try {
        const VnPreviewResult mapped =
            MapControl(harken::StopCurrentPlayable(internal, fadeMs));
        harken::RecordPreviewStopPlayable(handle, fadeMs, mapped);
        return mapped;
    } catch (...) {
        harken::RecordPreviewStopPlayable(
            handle, fadeMs, VN_PREVIEW_HOST_ERROR);
        return VN_PREVIEW_HOST_ERROR;
    }
}

VnPreviewResult VnPreview_QueryPlayable(VnPreviewPlayableHandle handle,
                                        VnPreviewPlayableStateV2* outState) {
    if (!kGateOpen) {
        harken::RecordPreviewQueryPlayable(
            handle, false, 0, VN_PREVIEW_DEVELOPMENT_API_DISABLED, nullptr);
        return VN_PREVIEW_DEVELOPMENT_API_DISABLED;
    }
    if (outState == nullptr) {
        harken::RecordPreviewQueryPlayable(
            handle, false, 0, VN_PREVIEW_INVALID_ARG, nullptr);
        return VN_PREVIEW_INVALID_ARG;
    }
    if (outState->structSize != sizeof(VnPreviewPlayableStateV2)) {
        harken::RecordPreviewQueryPlayable(
            handle, false, outState->structSize,
            VN_PREVIEW_STRUCT_SIZE_MISMATCH, nullptr);
        return VN_PREVIEW_STRUCT_SIZE_MISMATCH;
    }
    harken::PlayableInstanceHandle internal = 0;
    if (!ToInternalHandle(handle, &internal)) {
        harken::RecordPreviewQueryPlayable(
            handle, true, outState->structSize, VN_PREVIEW_INVALID_ARG,
            nullptr);
        return VN_PREVIEW_INVALID_ARG;
    }
    try {
        harken::PlayableControlState state{};
        const VnPreviewResult mapped =
            MapControl(harken::QueryCurrentPlayable(internal, &state));
        if (mapped != VN_PREVIEW_OK) {
            harken::RecordPreviewQueryPlayable(
                handle, true, outState->structSize, mapped, nullptr);
            return mapped;
        }
        VnPreviewPlayableStateV2 output{};
        output.structSize = sizeof(output);
        output.playableKind = static_cast<std::uint32_t>(state.kind);
        output.lifecycle = static_cast<std::uint32_t>(state.lifecycle);
        output.reason = static_cast<std::uint32_t>(state.reason);
        output.stableId = state.stableId;
        output.worldGeneration = state.worldGeneration;
        output.acceptedLogicTicket = state.acceptedLogicTicket;
        output.lastAppliedBlockSequence = state.lastAppliedBlockSequence;
        output.childLiveCount = state.childLiveCount;
        output.childAudibleRetiringCount =
            state.childAudibleRetiringCount;
        output.pendingFutureCount = state.pendingFutureCount;
        *outState = output;
        harken::RecordPreviewQueryPlayable(
            handle, true, outState->structSize, VN_PREVIEW_OK, &state);
        return VN_PREVIEW_OK;
    } catch (...) {
        harken::RecordPreviewQueryPlayable(
            handle, true, outState->structSize, VN_PREVIEW_HOST_ERROR,
            nullptr);
        return VN_PREVIEW_HOST_ERROR;
    }
}

}
