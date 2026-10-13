#ifndef HARKEN_PREVIEW_PREVIEW_ABI_H
#define HARKEN_PREVIEW_PREVIEW_ABI_H

#include <harken/ObjectId.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
#define VN_PREVIEW_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#define VN_PREVIEW_ALIGNOF(type) alignof(type)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define VN_PREVIEW_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define VN_PREVIEW_ALIGNOF(type) _Alignof(type)
#else
#error "harken_preview requires C11 or C++11"
#endif

#define HARKEN_PREVIEW_CAPABILITY_DEVELOPMENT_PREVIEW "development-preview"

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t VnPreviewResult;
typedef uint64_t VnPreviewPlayableHandle;

#define VN_PREVIEW_OK 0
#define VN_PREVIEW_INVALID_ARG 1
#define VN_PREVIEW_STRUCT_SIZE_MISMATCH 2
#define VN_PREVIEW_NOT_SUPPORTED 3
#define VN_PREVIEW_UNKNOWN_HANDLE 4
#define VN_PREVIEW_STALE_HANDLE 5
#define VN_PREVIEW_NO_PROGRAM 6
#define VN_PREVIEW_INSTANCE_CAPACITY 7
#define VN_PREVIEW_DEVELOPMENT_API_DISABLED 8
#define VN_PREVIEW_PROGRAM_MISMATCH 9
#define VN_PREVIEW_RESULT_CAPACITY 10
#define VN_PREVIEW_HOST_ERROR 11
#define VN_PREVIEW_EFFECT_CAPACITY 12
#define VN_PREVIEW_RESULT_EXPIRED 13
#define VN_PREVIEW_UNKNOWN_ID 14
#define VN_PREVIEW_NO_VOICE 15
#define VN_PREVIEW_BACKPRESSURE 16
#define VN_PREVIEW_REVISION_CONFLICT 17

#define VN_PREVIEW_PLAYABLE_KIND_SOUND 0
#define VN_PREVIEW_PLAYABLE_KIND_CONTAINER 1
#define VN_PREVIEW_PLAYABLE_KIND_TRACK 2
#define VN_PREVIEW_PLAYABLE_KIND_PIECE 3

#define VN_PREVIEW_LIFECYCLE_PREPARING 1
#define VN_PREVIEW_LIFECYCLE_ACTIVE 2
#define VN_PREVIEW_LIFECYCLE_CANCELLING 3
#define VN_PREVIEW_LIFECYCLE_LOGICAL_COMPLETE 4
#define VN_PREVIEW_LIFECYCLE_AUDIBLE_RETIRING 5
#define VN_PREVIEW_LIFECYCLE_FAILING_SOURCE_ACTIVE 6
#define VN_PREVIEW_LIFECYCLE_FAILING_AUDIBLE_RETIRING 7

#define VN_PREVIEW_REASON_NONE 0
#define VN_PREVIEW_REASON_NATURAL 1
#define VN_PREVIEW_REASON_STOP_REQUESTED 2
#define VN_PREVIEW_REASON_MEDIA_MISSING 3
#define VN_PREVIEW_REASON_CONTENT_REVISION 4
#define VN_PREVIEW_REASON_START_REFUSED 5
#define VN_PREVIEW_REASON_DEADLINE 6

typedef struct VnPreviewPlayPlayableDescV2 {
    uint32_t structSize;
    uint32_t playableKind;
    VnObjectId stableId;
    uint64_t emitterHandle;
    uint32_t flags;
    uint32_t reserved0;
} VnPreviewPlayPlayableDescV2;

typedef struct VnPreviewResetPlayableDescV2 {
    uint32_t structSize;
    uint32_t playableKind;
    VnObjectId stableId;
    uint64_t emitterHandle;
    uint32_t flags;
    uint32_t reserved0;
} VnPreviewResetPlayableDescV2;

typedef struct VnPreviewPlayableStateV2 {
    uint32_t structSize;
    uint32_t playableKind;
    uint32_t lifecycle;
    uint32_t reason;
    VnObjectId stableId;
    uint64_t worldGeneration;
    uint64_t acceptedLogicTicket;
    uint64_t lastAppliedBlockSequence;
    uint32_t childLiveCount;
    uint32_t childAudibleRetiringCount;
    uint32_t pendingFutureCount;
    uint32_t reserved0;
} VnPreviewPlayableStateV2;

#define VN_PREVIEW_PATH_ROW_CAPACITY 64u
#define VN_PREVIEW_PATH_SEND_CAPACITY 8u

typedef struct VnPreviewPlaybackPathRowV1 {
    VnObjectId stableId;
    uint32_t occurrenceOrdinal;
    uint32_t reserved0;
} VnPreviewPlaybackPathRowV1;

typedef struct VnPreviewPlaybackSendV1 {
    VnObjectId targetBusId;
    float levelDb;
    uint32_t reserved0;
} VnPreviewPlaybackSendV1;

typedef struct VnPreviewPlaybackPathV1 {
    uint32_t structSize;
    uint32_t rowCount;
    uint32_t sendCount;
    uint32_t reserved0;
    uint64_t voiceInstance;
    VnObjectId soundId;
    VnObjectId outputBusId;
    VnPreviewPlaybackPathRowV1 rows[VN_PREVIEW_PATH_ROW_CAPACITY];
    VnPreviewPlaybackSendV1 sends[VN_PREVIEW_PATH_SEND_CAPACITY];
} VnPreviewPlaybackPathV1;

VN_PREVIEW_STATIC_ASSERT(sizeof(VnPreviewPlaybackPathV1) == 1784,
                         "VnPreviewPlaybackPathV1 size");
VN_PREVIEW_STATIC_ASSERT(VN_PREVIEW_ALIGNOF(VnPreviewPlaybackPathV1) == 8,
                         "VnPreviewPlaybackPathV1 alignment");

VN_PREVIEW_STATIC_ASSERT(sizeof(VnPreviewPlayPlayableDescV2) == 40,
                         "VnPreviewPlayPlayableDescV2 size");
VN_PREVIEW_STATIC_ASSERT(VN_PREVIEW_ALIGNOF(VnPreviewPlayPlayableDescV2) == 8,
                         "VnPreviewPlayPlayableDescV2 alignment");
VN_PREVIEW_STATIC_ASSERT(sizeof(VnPreviewResetPlayableDescV2) == 40,
                         "VnPreviewResetPlayableDescV2 size");
VN_PREVIEW_STATIC_ASSERT(VN_PREVIEW_ALIGNOF(VnPreviewResetPlayableDescV2) == 8,
                         "VnPreviewResetPlayableDescV2 alignment");
VN_PREVIEW_STATIC_ASSERT(sizeof(VnPreviewPlayableStateV2) == 72,
                         "VnPreviewPlayableStateV2 size");
VN_PREVIEW_STATIC_ASSERT(VN_PREVIEW_ALIGNOF(VnPreviewPlayableStateV2) == 8,
                         "VnPreviewPlayableStateV2 alignment");

VnPreviewResult VnPreview_PlayPlayable(
    const VnPreviewPlayPlayableDescV2* desc,
    VnPreviewPlayableHandle* outHandle);
VnPreviewResult VnPreview_ResetPlayable(
    const VnPreviewResetPlayableDescV2* desc);
VnPreviewResult VnPreview_StopPlayable(VnPreviewPlayableHandle handle,
                                       uint32_t fadeMs);
VnPreviewResult VnPreview_QueryPlayable(VnPreviewPlayableHandle handle,
                                        VnPreviewPlayableStateV2* outState);
VnPreviewResult VnPreview_CapturePlaybackPaths(
    VnPreviewPlaybackPathV1* outPaths, uint32_t capacity, uint32_t* requiredCount);

#ifdef __cplusplus
}
#endif

#endif
