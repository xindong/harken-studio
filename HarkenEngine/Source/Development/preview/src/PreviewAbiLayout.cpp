#include "harken_preview/PreviewAbi.h"

#include <cstddef>

namespace {

static_assert(sizeof(VnPreviewPlaybackPathRowV1) == 24, "");
static_assert(alignof(VnPreviewPlaybackPathRowV1) == 8, "");
static_assert(offsetof(VnPreviewPlaybackPathRowV1, stableId) == 0, "");
static_assert(offsetof(VnPreviewPlaybackPathRowV1, occurrenceOrdinal) == 16, "");
static_assert(offsetof(VnPreviewPlaybackPathRowV1, reserved0) == 20, "");

static_assert(sizeof(VnPreviewPlaybackSendV1) == 24, "");
static_assert(alignof(VnPreviewPlaybackSendV1) == 8, "");
static_assert(offsetof(VnPreviewPlaybackSendV1, targetBusId) == 0, "");
static_assert(offsetof(VnPreviewPlaybackSendV1, levelDb) == 16, "");
static_assert(offsetof(VnPreviewPlaybackSendV1, reserved0) == 20, "");

static_assert(offsetof(VnPreviewPlaybackPathV1, structSize) == 0, "");
static_assert(offsetof(VnPreviewPlaybackPathV1, rowCount) == 4, "");
static_assert(offsetof(VnPreviewPlaybackPathV1, sendCount) == 8, "");
static_assert(offsetof(VnPreviewPlaybackPathV1, reserved0) == 12, "");
static_assert(offsetof(VnPreviewPlaybackPathV1, voiceInstance) == 16, "");
static_assert(offsetof(VnPreviewPlaybackPathV1, soundId) == 24, "");
static_assert(offsetof(VnPreviewPlaybackPathV1, outputBusId) == 40, "");
static_assert(offsetof(VnPreviewPlaybackPathV1, rows) == 56, "");
static_assert(offsetof(VnPreviewPlaybackPathV1, sends) == 1592, "");

static_assert(offsetof(VnPreviewPlayPlayableDescV2, structSize) == 0, "");
static_assert(offsetof(VnPreviewPlayPlayableDescV2, playableKind) == 4, "");
static_assert(offsetof(VnPreviewPlayPlayableDescV2, stableId) == 8, "");
static_assert(offsetof(VnPreviewPlayPlayableDescV2, emitterHandle) == 24, "");
static_assert(offsetof(VnPreviewPlayPlayableDescV2, flags) == 32, "");
static_assert(offsetof(VnPreviewPlayPlayableDescV2, reserved0) == 36, "");

static_assert(offsetof(VnPreviewResetPlayableDescV2, structSize) == 0, "");
static_assert(offsetof(VnPreviewResetPlayableDescV2, playableKind) == 4, "");
static_assert(offsetof(VnPreviewResetPlayableDescV2, stableId) == 8, "");
static_assert(offsetof(VnPreviewResetPlayableDescV2, emitterHandle) == 24, "");
static_assert(offsetof(VnPreviewResetPlayableDescV2, flags) == 32, "");
static_assert(offsetof(VnPreviewResetPlayableDescV2, reserved0) == 36, "");

static_assert(offsetof(VnPreviewPlayableStateV2, structSize) == 0, "");
static_assert(offsetof(VnPreviewPlayableStateV2, playableKind) == 4, "");
static_assert(offsetof(VnPreviewPlayableStateV2, lifecycle) == 8, "");
static_assert(offsetof(VnPreviewPlayableStateV2, reason) == 12, "");
static_assert(offsetof(VnPreviewPlayableStateV2, stableId) == 16, "");
static_assert(offsetof(VnPreviewPlayableStateV2, worldGeneration) == 32, "");
static_assert(offsetof(VnPreviewPlayableStateV2, acceptedLogicTicket) == 40, "");
static_assert(offsetof(VnPreviewPlayableStateV2, lastAppliedBlockSequence) == 48,
              "");
static_assert(offsetof(VnPreviewPlayableStateV2, childLiveCount) == 56, "");
static_assert(offsetof(VnPreviewPlayableStateV2,
                       childAudibleRetiringCount) == 60,
              "");
static_assert(offsetof(VnPreviewPlayableStateV2, pendingFutureCount) == 64,
              "");
static_assert(offsetof(VnPreviewPlayableStateV2, reserved0) == 68, "");

}
