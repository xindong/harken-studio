#ifndef HARKEN_HARKEN_DEVELOPMENT_H
#define HARKEN_HARKEN_DEVELOPMENT_H

#include <harken/Harken.h>
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

#define VN_REMOTE_DEFAULT_DISCOVERY_PORT 37840u

enum {
    VN_REMOTE_DISCOVERY_ENABLED = 1u,
    VN_REMOTE_STOPPED = 0u,
    VN_REMOTE_LISTENING = 1u,
    VN_REMOTE_CONNECTED = 2u,
    VN_REMOTE_FAILED = 3u
};

typedef struct VnRemoteConfig {
    uint32_t structSize;
    uint32_t tcpPort;
    uint32_t discoveryPort;
    uint32_t flags;
    const char* bindAddress;
    const char* applicationName;
    const char* projectName;
    uint32_t queueBytes;
    uint32_t maxCommandsPerUpdate;
    uint32_t requestTimeoutMs;
    uint32_t reserved0;
} VnRemoteConfig;

typedef struct VnRemoteStatus {
    uint32_t structSize;
    uint32_t state;
    uint32_t tcpPort;
    uint32_t discoveryPort;
    uint32_t processId;
    uint32_t protocolMajor;
    uint32_t protocolMinor;
    uint32_t clientCount;
    VnResult lastResult;
    uint32_t reserved0;
    uint64_t instanceId;
    uint64_t sessionId;
    uint64_t capabilities;
    uint64_t receivedCommands;
    uint64_t sentFrames;
    uint64_t droppedFrames;
    char bindAddress[64];
    char applicationName[128];
    char projectName[128];
    char engineVersion[32];
    char buildId[128];
    char platform[32];
} VnRemoteStatus;

/* Fills the caller's snapshot from the audio-side published double buffer
 * plus logic-owned counters. Never blocks the audio thread and never
 * touches audio-mutable state outside the published buffer. NULL or a
 * structSize mismatch is VN_INVALID_ARG; no engine is
 * VN_NOT_INITIALIZED. */
VnResult Vn_CaptureSnapshot(VnSnapshot* outSnapshot);

/* Legal before Vn_Init. Does not create an engine, install partitions, or
 * retain bytes after return. */
VnResult Vn_InspectInitPack(const VnPackLoadDesc* desc,
                            VnInitPackInspectReport* outReport);

/* Legal before Init; borrows immutable inputs only for this call. Uses the
 * shipping decoder and property fold. No engine or media installation occurs.
 * A zero names.projectId discovers the actual project from the Pack; a nonzero
 * value must match. Activation always requires an explicit project identity.
 * Zero capacities with null arrays query required counts. Otherwise every
 * capacity must suffice; VN_PACK_RESIDENT_CAPACITY writes only the report.
 * Unsupported preview is a successful inspection with zero occurrences.
 * Fixed gain is the authored source/output chain, not a mixed DSP result.
 * Only flagged duration/gain fields are known. No decoder pointers escape. */
VnResult Vn_InspectCanonicalPackV1(const VnPackInspectDescV1* desc,
                                  VnPackInspectReportV1* outReport);

/* Captures the generated built-in effect registry without allocation.
 * Always writes *requiredCount; writes min(capacity, requiredCount) rows and
 * returns VN_OK when truncated. A NULL requiredCount, a NULL out with nonzero
 * capacity, or an unknown row vintage is VN_INVALID_ARG. No initialized engine
 * is VN_NOT_INITIALIZED. */
VnResult Vn_CaptureEffectCapabilities(VnEffectCapability* out,
                                      uint32_t capacity,
                                      uint32_t* requiredCount);

/* Captures generated parameter metadata using the same buffer and refusal
 * contract as Vn_CaptureEffectCapabilities. */
VnResult Vn_CaptureEffectParameterCapabilities(
    VnEffectParameterCapability* out, uint32_t capacity,
    uint32_t* requiredCount);

VnResult Vn_CaptureEffectDynamicCapabilities(
    VnEffectDynamicCapability* out, uint32_t capacity,
    uint32_t* requiredCount);

VnResult Vn_CaptureEffectDynamicParameterCapabilities(
    VnEffectDynamicParameterCapability* out, uint32_t capacity,
    uint32_t* requiredCount);

VnResult Vn_CaptureEffectDynamicRoutes(VnEffectDynamicRouteInfo* out,
                                       uint32_t capacity,
                                       uint32_t* requiredCount);

VnResult Vn_CaptureCompressorRuntimeInfo(VnCompressorRuntimeInfo* out,
                                         uint32_t capacity,
                                         uint32_t* requiredCount);

/* Starts capturing a session trace into utf8Path (UTF-8, created or
 * truncated). Start gate: a trace must begin from a reconstructible origin,
 * so the call is accepted only before the first STATEFUL API call after
 * Vn_Init — stateful meaning anything that mutates engine content or
 * playback (content loads, bus creation, Action posts, value writes, replaces).
 * Queries (Vn_CaptureSnapshot, Vn_QueryActionFiring), Vn_Update, and
 * Vn_TraceStart itself are not stateful. A later start is refused
 * VN_TRACE_TOO_LATE: replaying a journal that begins mid-state would
 * require a full state bundle, a recorded leave-open for a future trace
 * format version. Refusals: NULL path is VN_INVALID_ARG; a trace already
 * capturing is VN_IN_USE; a file that cannot be created is
 * VN_BAD_TRACE_FILE (the engine is unaffected). An accepted start clears
 * the sticky traceFailed snapshot flag. A write failure mid-session stops
 * the trace, keeps the engine running, and surfaces as traceFailed plus a
 * log-ring record. API calls issued from inside a notification or log
 * callback are journaled at their real ordinal (inside the enclosing
 * Vn_Update's barrier pair) and are replayed after that update returns — a
 * session whose callbacks mutate state mid-update may replay with that
 * reordering; such traces are flagged, and the replayer completes them as
 * a LIMITED deterministic replay: it warns, still renders and captures,
 * and exits with a distinct status (0 = ordinary deterministic replay,
 * 3 = limited replay, 4 = content-hash refusal), so automation can never
 * read a limited run as ordinary proof of the recorded interleaving.
 * Logic thread only. */
VnResult Vn_TraceStart(const char* utf8Path);

/* Stops the active session trace, writes the end marker, flushes, and
 * closes the file. Stopping when no trace is active is a benign no-op
 * returning VN_OK. A flush failure here surfaces exactly like a mid-session
 * write failure (traceFailed + log record); the call still returns VN_OK.
 * Vn_Term with a trace still active performs this stop implicitly. Logic
 * thread only. */
VnResult Vn_TraceStop(void);

/* Captures one row per active property target — every bus or live-voice
 * target the property system is currently driving or has driven (any
 * combine slot active, a publication pending, a binding feeding it, or a
 * committed value differing from its base). Enumeration is deterministic
 * within one call — bus rows in creation order, then live-voice rows —
 * but the order is not a contract. Always writes *requiredCount; writes
 * min(capacity, requiredCount) rows and returns VN_OK even when
 * truncated (truncation is reported via *requiredCount, counted, and
 * logged with a named code). Refusals: a NULL requiredCount, a NULL out
 * with nonzero capacity, or an out[0].structSize that is not a known row
 * vintage is VN_INVALID_ARG; no engine is VN_NOT_INITIALIZED. Never
 * allocates; never touches audio-thread state. Logic thread only. */
VnResult Vn_CapturePropertyTargets(VnPropertyTargetInfo* out,
                                   uint32_t capacity,
                                   uint32_t* requiredCount);

/* Captures one row per binding attachment and per binding instance:
 * every attachment pool entry (bus and voice scope), every live voice's
 * definition-backed instance, and every instance identity held by a
 * parked voice or pinned by a pending-start voice (flagged PENDING).
 * Same buffer, truncation, refusal, and threading contract as
 * Vn_CapturePropertyTargets. */
VnResult Vn_CapturePropertyBindings(VnPropertyBindingInfo* out,
                                    uint32_t capacity,
                                    uint32_t* requiredCount);

VnResult Vn_MonitorRegisterHostV2(const VnMonitorHostRegistrationV2* desc);

VnResult Vn_MonitorBeginHostSession(VnMonitorHostSessionV1* outSession);

VnResult Vn_MonitorEndHostSession(uint64_t engineInstance, uint64_t sdkSession);

VnResult Vn_MonitorRegisterHost(const VnMonitorHostRegistrationV1* desc);

VnResult Vn_MonitorGetCapabilities(VnMonitorCapabilities* outCapabilities);

VnResult Vn_MonitorOpenSession(const VnMonitorSessionDesc* desc,
                               VnMonitorSessionHandle* outHandle);

VnResult Vn_MonitorCloseSession(VnMonitorSessionHandle handle);

VnResult Vn_MonitorOpenEventSession(const VnMonitorSessionDesc* desc,
                                    VnMonitorSessionHandle* outHandle);

VnResult Vn_MonitorSealEventSession(VnMonitorSessionHandle handle);

VnResult Vn_MonitorGetEventSessionStatus(VnMonitorSessionHandle handle,
                                         VnMonitorEventSessionStatus* outStatus);

VnResult Vn_MonitorGetSubscriptionStatus(VnMonitorSessionHandle handle,
                                         VnMonitorSubscriptionStatus* outStatus);

VnResult Vn_MonitorCaptureRegistry(uint32_t kind,
                                   void* out,
                                   uint32_t capacityBytes,
                                   uint32_t* requiredBytes,
                                   uint64_t* revision);

VnResult Vn_MonitorRequestStateCut(VnMonitorSessionHandle handle,
                                   const VnMonitorStateCutDesc* desc,
                                   uint64_t* outStateCutId);

VnResult Vn_MonitorGetStateCutStatus(VnMonitorSessionHandle handle,
                                     uint64_t stateCutId,
                                     VnMonitorStateCutStatus* outStatus);

VnResult Vn_MonitorAcquireCaptureControl(VnMonitorSessionHandle handle,
                                         VnMonitorCaptureControlStatus* outStatus);

VnResult Vn_MonitorReleaseCaptureControl(VnMonitorSessionHandle handle,
                                         uint64_t leaseGeneration);

VnResult Vn_MonitorGetCaptureControlStatus(VnMonitorSessionHandle handle,
                                           VnMonitorCaptureControlStatus* outStatus);

VnResult Vn_MonitorValidateCapturePlan(VnMonitorSessionHandle handle,
                                       uint64_t leaseGeneration,
                                       const VnMonitorCapturePlan* plan,
                                       VnMonitorEffectiveCapturePlan* outAccepted);

VnResult Vn_MonitorApplyCapturePlan(VnMonitorSessionHandle handle,
                                    uint64_t leaseGeneration,
                                    const VnMonitorCapturePlan* plan,
                                    VnMonitorEffectiveCapturePlan* outAccepted);

VnResult Vn_MonitorStopCapture(VnMonitorSessionHandle handle,
                               uint64_t leaseGeneration,
                               VnMonitorEffectiveCapturePlan* outResult);

VnResult Vn_MonitorGetEffectiveCapturePlan(VnMonitorSessionHandle handle,
                                           uint64_t capturePlanGeneration,
                                           VnMonitorEffectiveCapturePlan* outPlan);

VnResult Vn_MonitorPoll(VnMonitorSessionHandle handle,
                        const VnMonitorPollDesc* desc,
                        VnMonitorPollResult* outResult);

VnResult Vn_MonitorGetConsumerStatus(VnMonitorSessionHandle handle,
                                     VnMonitorDropSummary* outSummaries,
                                     uint32_t capacity,
                                     uint32_t* requiredCount);

VnResult Vn_MonitorStartRecording(VnMonitorSessionHandle handle,
                                  uint64_t leaseGeneration,
                                  const VnMonitorRecordingDesc* desc);

VnResult Vn_MonitorStopRecording(VnMonitorSessionHandle handle,
                                 uint64_t leaseGeneration);

VnResult Vn_MonitorGetRecordingStatus(VnMonitorSessionHandle handle,
                                      VnMonitorRecordingStatus* outStatus);

VnResult Vn_MonitorCompareSessions(const VnMonitorSessionCompareDesc* desc,
                                   VnMonitorCompareResult* out);

VnResult Vn_RemoteStart(const VnRemoteConfig* config);

VnResult Vn_RemoteStop(void);

VnResult Vn_RemoteGetStatus(VnRemoteStatus* outStatus);

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
#define VN_PREVIEW_PLAYABLE_KIND_SEGMENT 3

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

typedef uint64_t VnPreviewProjectHandle;

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
    float outputBusVolumeDb;
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
} /* extern "C" */
#endif

#endif /* HARKEN_HARKEN_DEVELOPMENT_H */
