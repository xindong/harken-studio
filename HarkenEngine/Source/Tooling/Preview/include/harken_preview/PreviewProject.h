#pragma once

#include <harken/Harken.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t VnPreviewProjectHandle;

/* Per-lane maxima since the preceding successful take on this engine.
 * consumeSerial identifies a consumption batch, not an audio block.
 * Lanes change windows independently. Only the Studio service pass consumes;
 * panels share its cache. Call on the logic thread. */
typedef struct VnPreviewChannelPeaksV1 {
    uint32_t structSize;
    uint32_t mixChannelCount;
    uint32_t busCount;
    uint32_t reserved0;
    uint64_t consumeSerial;
    VnBusId busIds[VN_SNAPSHOT_MAX_BUSES];
    float channelPeaks[VN_SNAPSHOT_MAX_BUSES * VN_MAX_CHANNELS];
} VnPreviewChannelPeaksV1;

typedef struct VnPreviewBusV1 {
    VnBusId id;
    VnBusId parent;
} VnPreviewBusV1;

typedef struct VnPreviewBusVolumeV2 {
    VnBusId id;
    float volumeDb;
    float makeUpGainDb;
    float outputBusVolumeDb;
    uint32_t reserved0;
} VnPreviewBusVolumeV2;

typedef struct VnPreviewBusSendV1 {
    VnBusId source;
    VnBusId target;
} VnPreviewBusSendV1;

typedef struct VnPreviewProjectDescV1 {
    uint32_t structSize;
    uint32_t bodyVintage;
    uint64_t projectSessionId;
    uint64_t authoringRevision;
    uint64_t runtimeEpoch;
    uint64_t submissionSerial;
    uint64_t publishedBusFingerprint;
    uint64_t compileTargetProfileFingerprint;
    uint64_t ownerId;
    uint64_t ownerBaseRevision;
    uint64_t ownerRevision;
    const uint8_t* body;
    uint64_t bodyBytes;
    const VnPreviewBusV1* buses;
    uint32_t busCount;
    uint32_t sendCount;
    const VnPreviewBusSendV1* sends;
} VnPreviewProjectDescV1;

typedef struct VnPreviewProjectDescV2 {
    VnPreviewProjectDescV1 base;
    const VnPreviewBusVolumeV2* busVolumes;
    uint32_t busVolumeCount;
    uint32_t reserved0;
    uint32_t profileVintage;
    uint32_t renderSampleRateHz;
    uint32_t mixLayout;
    uint32_t deviceLayout;
    uint64_t runtimeCapabilityMask;
} VnPreviewProjectDescV2;

typedef struct VnPreviewProjectDescV3 {
    VnPreviewProjectDescV2 base;
    const float* busSendLevelsDb;
    uint32_t busSendLevelCount;
    uint32_t reserved1;
} VnPreviewProjectDescV3;

typedef enum VnPreviewPrepareStatus {
    VN_PREVIEW_PREPARE_OK = 0,
    VN_PREVIEW_PREPARE_INVALID = 1,
    VN_PREVIEW_PREPARE_BACKPRESSURE = 2,
    VN_PREVIEW_PREPARE_CAPACITY = 3,
    VN_PREVIEW_PREPARE_UNSUPPORTED = 4,
    VN_PREVIEW_PREPARE_DEVICE_ERROR = 5,
    VN_PREVIEW_PREPARE_INVALID_WORLD = 6,
    VN_PREVIEW_PREPARE_UNAVAILABLE = 7
} VnPreviewPrepareStatus;

typedef enum VnPreviewCommitStatus {
    VN_PREVIEW_COMMIT_OK = 0,
    VN_PREVIEW_COMMIT_BACKPRESSURE = 1,
    VN_PREVIEW_COMMIT_INVALID = 2,
    VN_PREVIEW_COMMIT_DEVICE_ERROR = 3,
    VN_PREVIEW_COMMIT_UNAVAILABLE = 4
} VnPreviewCommitStatus;

VnPreviewPrepareStatus VnPreview_PrepareProject(
    const VnPreviewProjectDescV1* desc, VnPreviewProjectHandle* outHandle);
VnPreviewPrepareStatus VnPreview_PrepareProjectV2(
    const VnPreviewProjectDescV2* desc, VnPreviewProjectHandle* outHandle);
VnPreviewPrepareStatus VnPreview_PrepareProjectV3(
    const VnPreviewProjectDescV3* desc, VnPreviewProjectHandle* outHandle);
VnPreviewCommitStatus VnPreview_CommitProject(
    VnPreviewProjectHandle handle, uint64_t* outGeneration);
void VnPreview_CancelProject(VnPreviewProjectHandle handle);
uint32_t VnPreview_DrainProjectAcks(void);
uint32_t VnPreview_ProjectIsOpen(uint64_t generation);
uint32_t VnPreview_QueryOwnerRevision(uint64_t ownerId, uint64_t* outRevision);
uint32_t VnPreview_HasPlayableConsumers(void);
uint32_t VnPreview_HasAction(VnObjectId actionId);
void VnPreview_PumpNullMixer(uint32_t blockCount);
VnResult VnPreview_EnableChannelPeakMetering(void);
VnResult VnPreview_TakeChannelPeaks(VnPreviewChannelPeaksV1* outPeaks);
VnResult VnPreview_RegisterPcm(VnSoundId soundId, const VnPCMDesc* desc,
    uint64_t loopStartFrame, uint64_t loopEndFrame, uint64_t* outContentRevision);
VnResult VnPreview_PlayVoiceRaw(const VnPlayDesc* desc, VnVoiceHandle* outHandle);
VnResult VnPreview_RefreshVoiceBindings(uint64_t voiceInstance);
VnResult VnPreview_SetVoiceProperty(uint64_t voiceInstance,
    uint32_t property, float value, VnBusId sendBusId);

#ifdef __cplusplus
}
#endif
