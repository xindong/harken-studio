#ifndef HARKEN_HARKEN_DEVELOPMENT_H
#define HARKEN_HARKEN_DEVELOPMENT_H

#include <harken/Harken.h>
#include <stddef.h>
#include <stdint.h>

#define HARKEN_PREVIEW_CAPABILITY_DEVELOPMENT_PREVIEW "development-preview"

#ifdef __cplusplus
extern "C" {
#endif

/* Voice lifecycle state as reported in snapshots: INACTIVE covers both a
 * free slot and a voice that finished; ACTIVE is real source material
 * flowing; DRAINING is a finished source whose insert-effect tail is
 * still sounding; PREPARING is a streamed voice holding silent while its
 * ring fills to the prebuffer threshold. */
enum {
    VN_VOICE_STATE_INACTIVE = 0,
    VN_VOICE_STATE_ACTIVE = 1,
    VN_VOICE_STATE_DRAINING = 2,
    VN_VOICE_STATE_PREPARING = 3
};

/* Fixed snapshot reporting capacity. A snapshot reports the first
 * VN_SNAPSHOT_MAX_VOICES voice slots and the first VN_SNAPSHOT_MAX_BUSES
 * buses; engines configured larger still report true totals in
 * activeVoiceCount / busCount. */
#define VN_SNAPSHOT_MAX_VOICES 64
#define VN_SNAPSHOT_MAX_BUSES 32

/* One voice slot's observable state. `handle` is the slot index — the low
 * 32 bits of the VnVoiceHandle that owns the slot; pair it with `state`
 * to determine liveness. The four stage taps are the peak absolute sample
 * of the last rendered block at each cascade stage boundary (linear
 * amplitude). Tap measurement is compiled in for Debug builds and by
 * opt-in for Release; when compiled out the tap fields read 0.0f — the
 * struct layout never changes between builds. */
typedef struct VnVoiceSnapshot {
    uint32_t handle;       /* slot index (low 32 bits of the voice handle) */
    uint32_t state;        /* VN_VOICE_STATE_* */
    float tapPostResample; /* after source decode + resample */
    float tapPostFx;       /* after the insert effect */
    float tapPostFilter;   /* after the tone filter (LPF + HPF + shelf) */
    float tapPostGain;     /* after gain and fade: the cascade's output in
                            * source channels, measured before the bus
                            * fold-in applies any spatial pan/width gains */
} VnVoiceSnapshot;

/* Fixed row capacity of the per-type effect-pool stats table in VnSnapshot: it
 * carries one row per registered built-in effect type and has room for this
 * many, well above the current built-in count so appending a built-in effect
 * needs no snapshot layout change. */
#define VN_MAX_EFFECT_TYPE_STATS 16

/* One effect type's pool statistics. Explicit capacity is the configured quota;
 * automatic capacity is the number of fully backed rows, including free rows.
 * activeCount includes prepared, queued and retired-but-not-reclaimed instances.
 * highWater is the peak observed on live commit. refusalCount counts failed
 * acquisitions (quota, backing allocation or identity exhaustion). */
typedef struct VnEffectTypeStats {
    uint32_t effectType;   /* VnEffectType this row describes */
    uint32_t capacity;     /* explicit quota or automatic backing rows */
    uint32_t activeCount;  /* issued instances not yet reclaimed */
    uint32_t highWater;    /* most instances ever live at once */
    uint32_t refusalCount; /* cumulative acquisition refusals */
    uint32_t reserved0;    /* reserved, zero */
} VnEffectTypeStats;

/* Engine state snapshot filled by Vn_CaptureSnapshot. The render-side
 * fields (voice states, taps, bus peaks, arena marks, blockIndex) come
 * from a double buffer the audio thread publishes once per block; the
 * counter block below busPeaks merges in logic-owned counters at capture
 * time. busPeaks[i] is bus i's output peak for the last block, measured
 * after that bus's gain — index 0 is the master bus and reflects the
 * final mix (this is the bus meter). Like the voice taps, bus peaks read
 * 0.0f when tap measurement is compiled out; counters and voice states
 * are always live. */
typedef struct VnSnapshot {
    uint32_t structSize;       /* set to sizeof(VnSnapshot) before calling */
    uint32_t activeVoiceCount; /* Render Voices; true total, may exceed
                                * reported entries */
    uint64_t blockIndex;       /* index of the last published render block */
    VnVoiceSnapshot voices[VN_SNAPSHOT_MAX_VOICES]; /* indexed by slot */
    uint32_t busCount;         /* true total, may exceed reported entries */
    float busPeaks[VN_SNAPSHOT_MAX_BUSES]; /* dense bus index order */
    uint32_t reserved0;        /* keeps 8-byte alignment for the counters */
    uint64_t arenaHighWaterBytes; /* render arena peak use since init */
    uint64_t arenaOverflowCount;  /* failed render scratch allocations */
    uint64_t voiceHighWater;      /* most Render Voices ever live at once */
    uint64_t voiceCompletedCount; /* voices that finished or were stopped */
    uint64_t staleHandleCount;    /* operations refused on stale handles */
    uint64_t noVoiceCount;        /* render voice admission refusals recorded by the voice pool */
    uint64_t backpressureCount;   /* commands refused by a full queue */
    uint64_t notificationDropCount;      /* notifications lost to drop-oldest */
    uint64_t logDropCount;        /* audio-side log records lost to drop-oldest
                                    * (the logic-side ring has its own counter,
                                    * logicLogDropCount, at the tail) */
    uint64_t paramCommitCount;    /* resolved parameter values committed */
    uint64_t paramDropCount;      /* resolved values dropped as non-finite */
    uint64_t delayEffectHighWater; /* most feedback-delay instances live at once */
    uint64_t toneEffectHighWater;  /* most three-band-tone instances live at once */
    uint64_t packHighWater;   /* most packs ever loaded at once */
    uint64_t noPackCount;          /* pack loads refused by a full table */
    uint64_t streamHighWater;      /* most stream slots ever live at once */
    uint64_t streamRingHighWaterFrames; /* fullest any stream ring has been */
    uint64_t voiceStarvedCount;    /* starvation notifications emitted (cooldown-
                                    * aligned, not per silent block) */
    /* Playback-management additions (one struct-size vintage; appended, the
     * fields above keep their exact offsets). voicePublicHandles is index-
     * aligned with voices[]: entry i is the public voice handle currently
     * occupying render slot i, or 0 for a free/unowned slot. The voices[] rows
     * keep reporting render-slot indices, so this array is the supported way
     * to join a public handle to a render-slot observability row. The counters
     * that follow are logic-owned and read 0 until the feature that owns each
     * one ships. */
    uint64_t voicePublicHandles[VN_SNAPSHOT_MAX_VOICES];
    uint64_t parkedVoiceCount;     /* current Virtual Voice occupancy */
    uint64_t voiceStolenCount;     /* voices ended by steal policy */
    uint64_t voiceParkedCount;     /* cumulative park transitions */
    uint64_t voiceReturnedCount;   /* cumulative return transitions */
    uint64_t returnRefusedCount;   /* returns refused by capacity/steal */
    uint64_t stepSkippedCount;   /* reserved legacy field; always 0 */
    uint64_t actionHighWater;      /* reserved legacy field; always 0 */
    uint64_t containerHighWater;   /* reserved legacy field; always 0 */
    uint64_t emitterHighWater;     /* most emitters ever live at once */
    uint64_t pendingActionHighWater; /* reserved legacy field; always 0 */
    uint64_t parkedHighWater;      /* most voices ever parked at once */
    uint64_t sendSlotHighWater;    /* most send slots ever in use at once */
    uint64_t containerChildHighWater; /* reserved legacy field; always 0 */
    uint64_t containerCursorHighWater; /* reserved legacy field; always 0 */
    uint64_t topologyPlanHighWater;    /* most topology plans live at once */
    uint64_t containerResolutionFailCount; /* reserved legacy field; always 0 */
    uint64_t logicNotificationDropCount; /* logic-side notifications lost to
                                          * drop-oldest */
    uint64_t topologyPlanCount;    /* topology plans published */
    uint64_t droppedDeadEmitterStepCount; /* reserved legacy field; always 0 */
    uint64_t pendingStartCount;    /* Logical Voices currently deferred before
                                     * their start; not Virtual Voices */
    uint64_t parkDeferredCount;    /* parks deferred by a stale source cursor */
    uint64_t noActionCount;        /* reserved legacy field; always 0 */
    uint64_t noContainerCount;     /* reserved legacy field; always 0 */
    uint64_t noEmitterCount;       /* emitter creations refused by capacity */
    uint64_t noSendCount;          /* send connections refused by capacity */
    uint64_t triggerRefusedCount;  /* reserved legacy field; always 0 */
    uint64_t topologyRefusedCount; /* topology changes refused by pool/admission */
    /* A further struct-size vintage, appended so every field above keeps its
     * exact offset. A caller whose struct predates this field simply does not
     * receive it. */
    uint32_t logicLogDropCount;    /* logic-side log records lost to drop-oldest */
    /* Action-extension, hot-reload, tracing, and per-stage-timing additions
     * (one struct-size vintage, appended so every field above keeps its exact
     * offset). Counters are logic-owned except the audio timing quintet, which
     * the audio thread publishes through the existing snapshot double buffer. */
    uint32_t reserved1;            /* keeps 8-byte alignment for the counters */
    uint64_t nestedTriggerDroppedCount; /* reserved legacy field; always 0 */
    uint64_t triggerDepthHighWater;     /* reserved legacy field; always 0 */
    uint64_t hotReloadCount;            /* successful replace calls, cumulative */
    uint64_t hotReloadRefusedCount;     /* replace calls refused, cumulative */
    uint64_t pinnedGenerationCount;     /* asset generations currently retiring:
                                         * old content pinned until its last
                                         * consuming voice drains */
    uint64_t pinnedGenerationHighWater; /* most generations ever retiring at once */
    uint32_t traceActive;               /* 1 while a session trace is capturing */
    uint32_t traceFailed;               /* 1 after a mid-session trace write
                                         * failure; sticky until the next trace
                                         * start */
    uint32_t audioCpuLastUs;            /* audio callback CPU for the last
                                         * rendered block, microseconds.
                                         * Contract for the audio stage trio
                                         * below: drain + voices + buses <=
                                         * this whole, within a documented
                                         * measurement tolerance of 2 us for
                                         * clock-boundary noise (the stages
                                         * are measured from shared clock
                                         * reads inside the same callback) */
    uint32_t audioCpuPeakUs;            /* audio callback CPU peak since init */
    uint32_t audioDrainLastUs;          /* command-drain stage of the last block */
    uint32_t audioVoicesLastUs;         /* voice-cascade stage of the last block */
    uint32_t audioBusesLastUs;          /* bus walk + output stage of the last
                                         * block */
    uint32_t updateCpuLastUs;           /* Vn_Update CPU for the last pump,
                                         * microseconds */
    uint32_t updateCpuPeakUs;           /* Vn_Update CPU peak since init */
    uint32_t reserved2;                 /* keeps 8-byte alignment for future
                                         * appended fields */
    /* Multichannel-output and job-parallel observability (one struct-size
     * vintage, appended so every field above keeps its exact offset; a caller
     * whose struct predates them does not receive them). The session facts
     * (mix/device width, downmix state, worker count) are logic-owned; the
     * worker busy peak is audio-side-measured and published through the
     * snapshot double buffer with the timing fields above. */
    uint32_t mixChannelCount;           /* active mix-layout channel count */
    uint32_t deviceChannelCount;        /* active device channel count */
    uint32_t outputDownmixActive;       /* 0/1: whether the master->device
                                         * output downmix runs this session
                                         * (device narrower than the mix) */
    uint32_t renderWorkerCount;         /* active render-worker count (0 =
                                         * serial in-callback rendering) */
    uint32_t renderWorkerBusyPeakUs;    /* peak per-block busy time across
                                         * render workers since init,
                                         * microseconds (0 while serial) */
    uint32_t reserved3;                 /* keeps 8-byte alignment for future
                                         * appended fields */
    /* Property/binding observability (one struct-size vintage, appended so
     * every field above keeps its exact offset; a caller whose struct
     * predates them does not receive them). All logic-owned. The three
     * binding pools report a high-water mark and a refusal counter each;
     * a refusal here always has a paired log record naming the pool. */
    uint64_t bindingDefinitionHighWater;   /* most definitions registered at
                                            * once */
    uint64_t bindingDefinitionRefusedCount; /* registrations refused by a
                                             * full definition pool */
    uint64_t bindingAttachmentHighWater;   /* most attachments live at once */
    uint64_t bindingAttachmentRefusedCount; /* attaches refused by a full
                                             * attachment pool */
    uint64_t bindingInstanceHighWater;     /* fullest the shared voice
                                            * instance table has been */
    uint64_t bindingInstanceRefusedCount;  /* instance creations refused by
                                            * the per-voice slot ceiling or
                                            * the shared table bound (play
                                            * acceptance refusals included) */
    uint64_t paramPublicationDeferredCount; /* whole-batch property
                                             * publications deferred for lack
                                             * of command-queue room */
    uint64_t meterRingOverflowCount;       /* cumulative meter blocks
                                            * max-hold compressed by a full
                                            * transport ring, as observed by
                                            * the update drain */
    uint64_t meterLoopCount;               /* post-wiring control-loop freeze
                                            * transitions (bindings frozen
                                            * neutral) */
    /* Per-type effect-pool observability (one struct-size vintage, appended so
     * every field above keeps its exact offset; a caller whose struct predates
     * these does not receive them). effectTypeStatCount is the number of
     * populated rows (the registered built-in effect-type count); rows
     * [0, effectTypeStatCount) are valid in effect-type order and the rest are
     * zeroed. The delayEffectHighWater / toneEffectHighWater scalars above stay
     * and equal their rows' highWater. All logic-owned. */
    uint32_t effectTypeStatCount;  /* populated rows in effectTypeStats */
    uint32_t reserved4;            /* keeps 8-byte alignment / reserved, zero */
    VnEffectTypeStats effectTypeStats[VN_MAX_EFFECT_TYPE_STATS];
    /* Bus audibility masks (one struct-size vintage, appended so every field
     * above keeps its exact offset; a caller whose struct predates these does
     * not receive them). Bit i describes dense bus index i
     * (VN_SNAPSHOT_MAX_BUSES fits uint32_t): busMuteMask is the authored
     * per-bus mute state (Vn_SetBusMute), busSoloEffectiveMask is post-solo
     * effective audibility — bit set = the bus is audible (not muted, and on
     * a solo path or no solo active). Logic-owned, merged at capture. */
    uint32_t busMuteMask;
    uint32_t busSoloEffectiveMask;
    /* Reserved legacy field; always 0. */
    uint64_t actionEmitterMultichannelFallbackCount;
    /* Bus meter tap peaks (one struct-size vintage, appended so every field
     * above keeps its exact offset). Dense bus index order; callers whose
     * snapshot predates these fields do not receive them. */
    float busPeaksPreFader[VN_SNAPSHOT_MAX_BUSES];
    float busPeaksPostFader[VN_SNAPSHOT_MAX_BUSES];
    /* Effect-local Dynamic Route observability (one append-only snapshot
     * vintage). Current/capacity/high-water values and logic-side refusals
     * merge at capture time. Audio-owned diagnostics ride the same coherent
     * publication as the block counters above. */
    uint32_t dynamicRouteCurrent;
    uint32_t dynamicRouteCapacity;
    uint32_t dynamicRouteHighWater;
    uint32_t dynamicRoutePlanCurrent;
    uint32_t dynamicRoutePlanCapacity;
    uint32_t dynamicRoutePlanHighWater;
    uint32_t dynamicBusTapCurrent;
    uint32_t dynamicBusTapCapacity;
    uint32_t dynamicBusTapHighWater;
    uint32_t dynamicVoiceTapCurrent;
    uint32_t dynamicVoiceTapCapacity;
    uint32_t dynamicVoiceTapHighWater;
    uint64_t dynamicRouteRefusalCount;
    uint64_t dynamicRouteCycleCount;
    uint64_t dynamicRouteOrderRefusalCount;
    uint64_t dynamicRouteStaleReadCount;
    uint64_t dynamicTapGenerationMismatchCount;
    uint64_t dynamicPlanPublicationCount;
    uint64_t dynamicPlanRetireOverflowCount;
    uint64_t compressorDetectorNonFiniteCount;
    uint64_t compressorDetectorSaturationCount;
    /* Atomic Bus topology observability. busIds/busGenerations follow the
     * existing dense slot span reported by busCount; dead/free rows are zero. */
    uint32_t activeBusCount;
    uint32_t freeBusSlotCount;
    uint32_t quarantinedBusSlotCount;
    uint32_t reserved5;
    uint64_t busSlotReuseCount;
    uint64_t acceptedBusTopologyRevision;
    uint64_t installedBusTopologyRevision;
    uint64_t topologyPlanByteBudget;
    uint64_t topologyPlanPoolBytes;
    VnBusId busIds[VN_SNAPSHOT_MAX_BUSES];
    uint32_t busGenerations[VN_SNAPSHOT_MAX_BUSES];
    VnResult lastTopologyRefusalResult;
    uint32_t lastTopologyRefusalCategory;
    uint64_t lastTopologyRefusalOwner;
    VnBusId lastTopologyRefusalBusId;
    VnBusId lastTopologyRefusalRelatedBusId;
    /* Most control commands one block has ever had to leave for a later block.
     * Commands over the per-block ceiling are applied later rather than dropped,
     * so a backlog shows up as latency with nothing else naming it: a rising
     * value here means control changes are landing later than they were issued.
     * Zero for a session that never exceeded the ceiling. */
    uint64_t commandDeferredCount;
    /* Diagnostic-transport occupancy (one struct-size vintage). Both are the
     * fullest their ring has ever been observed at a drain, in records,
     * saturated at the ring's capacity. The drop counters beside them
     * (logDropCount, notificationDropCount) report loss after it happened,
     * which is too late to size a depth from; these report the headroom that
     * remained while nothing was lost, so a caller can tell "never came close"
     * from "survived by one record". Zero for a session whose drains always
     * found the ring empty. The log ring's capacity is a compile-time constant
     * and not configurable; the notification queue's is notificationQueueDepth
     * rounded up to a power of two. */
    uint32_t logRingHighWater;
    uint32_t notificationQueueHighWater;
    /* Per-quantum producer budget (one struct-size vintage). One render block
     * may produce at most its transport's capacity in records; production past
     * that is dropped, never deferred, because a log record and a voice event
     * both describe a moment in the block that produced them.
     *
     * These are deliberately not folded into logDropCount /
     * notificationDropCount. Those count records the producer overwrote before
     * the consumer read them -- the consumer drained too late. These count
     * records one block was refused for producing too much. The two have
     * opposite fixes (drain more often versus produce less), so one number could
     * not tell a caller which they have.
     *
     * The produced high-waters are likewise not the occupancy high-waters above:
     * occupancy depends on when the consumer ran, production does not. These are
     * the figures the budget is sized against. Refusals are excluded -- a
     * refused record was never produced. */
    uint64_t logQuantumBudgetDropCount;
    uint64_t notificationQuantumBudgetDropCount;
    uint32_t logQuantumProducedHighWater;
    uint32_t notificationQuantumProducedHighWater;
    uint64_t deadlineMissQuanta;
    uint64_t lateConsumedEvents;
    uint64_t variableAcceptedCount;
    uint64_t variableAppliedCount;
    uint64_t variableSupersededCount;
    uint64_t variablePendingCount;
    uint64_t variableTerminationCount;
    float busChannelPeaks[VN_SNAPSHOT_MAX_BUSES * VN_MAX_CHANNELS];
    uint32_t vorbisDecodeLastUs;
    uint32_t vorbisDecoderCount;
} VnSnapshot;

typedef uint32_t VnCompressorRuntimeFlags;
enum {
    VN_COMPRESSOR_RUNTIME_BYPASSED = 1u << 0,
    VN_COMPRESSOR_RUNTIME_BYPASS_TRANSITION = 1u << 1,
    VN_COMPRESSOR_RUNTIME_PARAM_TRANSITION = 1u << 2,
    VN_COMPRESSOR_RUNTIME_EXTERNAL_AUTHORED = 1u << 3,
    VN_COMPRESSOR_RUNTIME_EXTERNAL_ACTIVE = 1u << 4,
    VN_COMPRESSOR_RUNTIME_RESET_THIS_BLOCK = 1u << 5,
    VN_COMPRESSOR_RUNTIME_STALE = 1u << 6,
    VN_COMPRESSOR_RUNTIME_RESET_BRIDGE = 1u << 7
};

typedef struct VnEffectDynamicCapability {
    uint32_t structSize;
    uint32_t reserved0;
    uint32_t effectType;
    uint32_t acceptedSignalFormsMask;
    uint32_t allowedSourceKindsMask;
    uint32_t allowedBusTapsMask;
    uint32_t allowedVoiceTapsMask;
    uint32_t hostScopeMask;
    uint32_t maxRawRoutesPerInstance;
    uint32_t configuredGlobalRouteCapacity;
    uint32_t configuredBusTapCapacity;
    uint32_t configuredVoiceTapCapacity;
    uint32_t rawBusLatencyBlocks;
    uint32_t rawVoiceLatencyBlocks;
    uint32_t flags;
    uint32_t descriptorVintage;
    float rawRouteLevelMinDb;
    float rawRouteLevelMaxDb;
    float rawRouteLevelDefaultDb;
    uint32_t rawRouteCombine;
    uint32_t rawChannelReduction;
    uint32_t reserved1;
} VnEffectDynamicCapability;

typedef struct VnEffectDynamicParameterCapability {
    uint32_t structSize;
    uint32_t reserved0;
    uint32_t effectType;
    uint32_t parameterIndex;
    uint32_t acceptedSignalFormsMask;
    uint32_t sourceKindsMask;
    uint32_t updateCadence;
    uint32_t smoothingOwner;
    uint32_t smoothingFrames;
    uint32_t flags;
} VnEffectDynamicParameterCapability;

typedef struct VnEffectDynamicRouteInfo {
    uint32_t structSize;
    uint32_t reserved0;
    VnObjectId targetBusId;
    VnObjectId sourceId;
    uint64_t sourceVoiceHandle;
    uint64_t revision;
    uint64_t blockEpoch;
    uint32_t targetEffectType;
    uint32_t effectSlot;
    uint32_t routeSlot;
    uint32_t signalForm;
    uint32_t sourceKind;
    uint32_t tap;
    float levelDb;
    uint32_t stateFlags;
    uint32_t inactiveReason;
    uint32_t ownerKind;
    uint32_t pendingPublication;
    uint32_t reserved1;
} VnEffectDynamicRouteInfo;

typedef struct VnCompressorRuntimeInfo {
    uint32_t structSize;
    uint32_t reserved0;
    VnObjectId objectId;
    uint64_t instanceHandle;
    uint64_t blockEpoch;
    uint64_t detectorNonFiniteCount;
    uint64_t detectorSaturationCount;
    uint32_t hostScope;
    uint32_t effectSlot;
    uint32_t stateFlags;
    uint32_t transitionFramesRemaining;
    uint32_t staleRouteCount;
    float mainInputPeakDb;
    float detectorPeakDb;
    float outputPeakDb;
    float maxGainReductionDb;
    float endGainReductionDb;
} VnCompressorRuntimeInfo;

/* One deterministic discovery row per built-in effect type. descriptorSize is
 * the current concrete descriptor size and descriptorVintage identifies its
 * authored schema. parameterCount names the effect-private index space.
 * reserved fields are zero. */
typedef struct VnEffectCapability {
    uint32_t structSize; /* caller sets out[0].structSize =
                          * sizeof(VnEffectCapability) when capacity > 0 */
    uint32_t effectType;
    uint32_t descriptorSize;
    uint32_t descriptorVintage;
    uint32_t parameterCount;
    uint32_t hostScopeMask;
    uint32_t reserved0;
    uint32_t reserved1;
} VnEffectCapability;

/* One deterministic discovery row per built-in effect parameter, ordered by
 * effect type then private parameter index. neutral is the identity
 * contribution for the declared binding-combine domain, not the effect's
 * authored/default parameter value. clampMin/clampMax are the resolved bounds
 * for this initialized engine: open-positive resolves to the smallest positive
 * float, open-one to std::nextafter(1.0f, 0.0f), and Nyquist to the largest
 * float below 0.5 * sampleRate. All reported bounds are concrete finite
 * values; registry sentinels never cross the API. */
typedef struct VnEffectParameterCapability {
    uint32_t structSize; /* caller sets out[0].structSize =
                          * sizeof(VnEffectParameterCapability) */
    uint32_t effectType;
    uint32_t parameterIndex;
    uint32_t unit;
    uint32_t combine;
    uint32_t flags;
    float neutral;
    float clampMin;
    float clampMax;
    uint32_t reserved0;
} VnEffectParameterCapability;

/* Property attribution capture: two logic-side calls that explain the
 * property system's current state — "why is Music at -18 dB?" — as data.
 * Both fill CALLER-PROVIDED row buffers and never allocate. Both always
 * write *requiredCount (the total row count the current state describes);
 * when requiredCount exceeds a nonzero capacity the call still succeeds,
 * writes the first `capacity` rows, and reports the truncation honestly:
 * the caller sees requiredCount > capacity, the engine counts the
 * truncation and logs a named record through the log ring. The two-call
 * idiom is capacity 0 (out may be NULL) to probe the size, then a sized
 * fetch; a capacity-0 probe is the sizing idiom, not a truncation, and
 * is neither counted nor logged.
 *
 * Rows describe the LOGIC THREAD'S CURRENT STATE at the moment of the
 * call: slot values and contributions are the last realized evaluation
 * (the state feeding the most recent publication), so target rows and
 * binding rows are mutually consistent; a Variable written since the
 * last Vn_Update shows its effect only after the next one. Logic thread
 * only. Row shapes are frozen; later fields append under a new row
 * structSize vintage. */

/* State flags of one VnPropertyBindingInfo row (bitmask). ACTIVE marks a
 * row that is in force and evaluating: a bus attachment contributing, a
 * live voice instance (whether or not its Variable has resolved a value
 * yet — an unset input just contributes 0), or a voice-scope attachment
 * armed for future plays. NEUTRALIZING marks a contribution on its way
 * to neutral and no longer tracking inputs: a cleared/detached instance,
 * or a bus attachment frozen at neutral by the post-wiring meter-loop
 * rule; a just-neutralized instance still reports its in-force
 * contribution until the next update's exit evaluation realizes the
 * departure. PENDING marks an instance identity a parked voice holds or
 * a pending-start voice has pinned — not currently evaluating, live
 * again at return/onset. */
enum {
    VN_PROPERTY_BINDING_ACTIVE = 1,
    VN_PROPERTY_BINDING_NEUTRALIZING = 2,
    VN_PROPERTY_BINDING_PENDING = 4
};

/* One active property target's attribution row: the scoped target locator
 * (scopeKind, objectId, targetId — objectId is the public VnBusId for BUS
 * rows and the public voice handle for VOICE rows), the target's base
 * (the authored per-voice base, or the bus target's neutral base), the
 * two combine-slot values the property system owns, the final resolved
 * value the audio thread was (or will be) told, and the publication
 * facts. apiDirectValue/variableValue read as the target's neutral
 * element while their slot is untouched; finalValue is the full
 * reduction, so for VOICE rows it also folds the engine-owned
 * attenuation contribution. pendingPublication is 1 while a change is
 * staged or known-incoming but not yet committed (deferred batches
 * included). activeBindingCount counts the attachments/instances
 * currently feeding the target. lastPublishedRampMs is the rampMs the
 * target's most recent committed command carried (0 before any). */
typedef struct VnPropertyTargetInfo {
    uint32_t structSize; /* caller sets out[0].structSize =
                          * sizeof(VnPropertyTargetInfo) when capacity > 0;
                          * the engine stamps every written row */
    uint32_t scopeKind;  /* VN_TARGET_SCOPE_VOICE / _BUS */
    VnObjectId objectId;   /* public VnBusId, or public voice handle */
    uint64_t instanceHandle;
    uint32_t targetId;   /* scoped target table id */
    uint32_t pendingPublication;  /* 0/1 */
    float base;                   /* reduction's starting value */
    float apiDirectValue;         /* API-direct slot (neutral if untouched) */
    float variableValue;          /* Variable slot's reduced accumulation */
    float finalValue;             /* full reduction, clamps applied */
    uint32_t activeBindingCount;  /* attachments/instances feeding this row */
    uint32_t lastPublishedRampMs; /* rampMs of the last committed command */
} VnPropertyTargetInfo;

/* One binding attachment's or instance's attribution row. bindingId and
 * variableId are the caller-minted identities — tools map them back to
 * authored names; the runtime never re-learns those nouns.
 * definitionRevision is the revision the row's numbers were computed
 * with: for instance rows the revision last evaluated, for
 * attachment/held rows the currently installed one. objectId names the
 * row's object: the attached bus id (BUS attachments), the attached
 * playable id (VOICE attachments — armed for future plays), or the
 * public voice handle (instances, parked and pending-start identities
 * included). currentInput is the Variable's value resolved at the row's
 * scope RIGHT NOW (0 while the Variable has no value at any scope — a
 * reported 0 with ACTIVE set and a 0 contribution is how an armed,
 * never-driven instance reads). currentContribution is the curve output
 * in the target's domain: for instance rows the last REALIZED
 * contribution — the term the target rows' Variable slot actually
 * folds, so binding rows sum to the target rows' numbers in every
 * phase: a just-neutralized instance keeps reporting its in-force value
 * (NEUTRALIZING) until the next update's exit evaluation zeroes it, an
 * instance whose Variable never resolved reports 0, and
 * pendingPublication marks a not-yet-realized input change. 0 for
 * frozen bus attachments and for voice-scope attachment rows (only
 * instances contribute); for PENDING held identities the contribution
 * the current input would apply at return/onset. transitionMs is the
 * definition's current ramp request. */
typedef struct VnPropertyBindingInfo {
    uint32_t structSize; /* caller sets out[0].structSize =
                          * sizeof(VnPropertyBindingInfo) when capacity > 0;
                          * the engine stamps every written row */
    uint32_t stateFlags; /* VN_PROPERTY_BINDING_* bitmask */
    VnObjectId bindingId;
    VnObjectId variableId;
    uint32_t scopeKind;         /* definition's target scope */
    uint32_t targetId;          /* definition's target id */
    VnObjectId objectId;          /* bus id / Sound id / voice handle */
    uint64_t instanceHandle;
    uint32_t definitionRevision;
    uint32_t transitionMs;
    float currentInput;         /* Variable value at the row's scope */
    float currentContribution;  /* curve output, target domain */
} VnPropertyBindingInfo;

/* Runtime Monitor public contract, first vintage. Result codes and this
 * version of each VnMonitor* layout are frozen; later Monitor fields may
 * only append. Callers pass structSize equal to a known vintage prefix;
 * unknown sizes are rejected. Reserved fields must be caller-zeroed;
 * a live implementation writes reserved fields back to 0.
 * Unimplemented Monitor entries do not inspect arguments and never write
 * caller buffers: no engine -> VN_NOT_INITIALIZED; engine present ->
 * VN_MONITOR_UNSUPPORTED. Implemented Hub entries (Open/Close/Poll/
 * capabilities/subscription/consumer status) inspect structSize and may
 * succeed or write caller buffers.
 * Enums are uint32_t. Records are 8-byte aligned.
 * Vn_MonitorCaptureRegistry takes uint32_t kind because the binding u32
 * validator currently accepts only uint32_t or VnEffectType; values are
 * VnMonitorRegistryKind.
 * Wire integers are little-endian and must not be a native memcpy of these
 * C structs.
 * deliverySequence in VnMonitorRecordHeader is consumer-local: Remote
 * transcripts store 0; only Vn_MonitorPoll fills it.
 * Names, paths, and labels do not enter RT records. Numeric 0 is silence
 * only when a record is armed and validity flags say the sample is valid.
 * Record flags occupy bits 0-1 of VnMonitorRecordHeader.flags
 * (REQUIRED / OPTIONAL). Remaining bits must be zero in this vintage.
 * Packed on-wire record envelopes are little-endian and sized by
 * VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES; do not memcpy the in-memory
 * VnMonitorRecordHeader. Payload structs below are the first vintage of
 * State/Event/measurement layouts; later fields may only append. */

typedef uint64_t VnMonitorSessionHandle;
typedef uint32_t VnMonitorRegistryKind;
typedef uint32_t VnMonitorStreamKind;
typedef uint32_t VnMonitorRecordKind;
typedef uint32_t VnMonitorDropStage;
typedef uint32_t VnMonitorEntityKind;
typedef uint32_t VnMonitorTapKind;
typedef uint32_t VnMonitorTapSelectorMode;
typedef uint32_t VnMonitorPermissionMask;
typedef uint32_t VnMonitorSecurityProfile;
typedef uint32_t VnMonitorPlanActivationState;
typedef uint32_t VnMonitorStateCutResult;
typedef uint32_t VnMonitorMeasurementKind;
typedef uint32_t VnMonitorValidityFlags;
typedef uint32_t VnMonitorRemoteOpcode;
typedef uint32_t VnMonitorChannelMode;

enum {
    VN_MONITOR_STREAM_STATE = 1u << 0,
    VN_MONITOR_STREAM_EVENT = 1u << 1,
    VN_MONITOR_STREAM_TIME_SERIES = 1u << 2,
    VN_MONITOR_STREAM_OPTIONAL_AUDIO = 1u << 3
};

enum {
    VN_MONITOR_RECORD_UNKNOWN = 0,
    VN_MONITOR_RECORD_STATE_CUT_BEGIN = 1,
    VN_MONITOR_RECORD_STATE_UPSERT = 2,
    VN_MONITOR_RECORD_STATE_TOMBSTONE = 3,
    VN_MONITOR_RECORD_STATE_CUT_END = 4,
    VN_MONITOR_RECORD_EVENT = 5,
    VN_MONITOR_RECORD_TIME_SERIES = 6,
    VN_MONITOR_RECORD_AUDIO_CHUNK = 7,
    VN_MONITOR_RECORD_WAVEFORM_BUCKET = 8,
    VN_MONITOR_RECORD_SCOPE_RESULT = 9,
    VN_MONITOR_RECORD_SPECTRUM = 10,
    VN_MONITOR_RECORD_METER = 11,
    VN_MONITOR_RECORD_GAIN_REDUCTION = 12,
    VN_MONITOR_RECORD_DROP_SUMMARY = 13,
    VN_MONITOR_RECORD_STREAM_BASELINE = 14,
    VN_MONITOR_RECORD_CAPTURE_PLAN_ACTIVATED = 15,
    VN_MONITOR_RECORD_TAP_BINDING_CHANGED = 16,
    VN_MONITOR_RECORD_PARTIAL_BUCKET_DISCARDED = 17,
    VN_MONITOR_RECORD_AGGREGATE = 18,
    VN_MONITOR_RECORD_MEASUREMENT_SEGMENT_RESET = 19,
    VN_MONITOR_RECORD_CLOCK_ANCHOR = 20,
    VN_MONITOR_RECORD_REGISTRY_REVISION = 21,
    VN_MONITOR_RECORD_INVALID = 22,
    VN_MONITOR_RECORD_EVENT_DETAIL = 23,
    VN_MONITOR_RECORD_EXECUTION_FACT = 24,
    VN_MONITOR_RECORD_IDENTITY_FACT = 25,
    VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN = 26,
    VN_MONITOR_RECORD_IDENTITY_CUT_END = 27,
    VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL = 28
};

enum {
    VN_MONITOR_DROP_SOURCE_PRODUCER = 1,
    VN_MONITOR_DROP_HUB_FANOUT = 2,
    VN_MONITOR_DROP_REMOTE_TRANSPORT = 4,
    VN_MONITOR_DROP_DERIVED_INPUT = 5
};

enum {
    VN_MONITOR_ENTITY_NONE = 0,
    VN_MONITOR_ENTITY_SESSION = 1,
    VN_MONITOR_ENTITY_PLAYABLE = 2,
    VN_MONITOR_ENTITY_ACTION = 3,
    VN_MONITOR_ENTITY_VOICE = 4,
    VN_MONITOR_ENTITY_BUS = 5,
    VN_MONITOR_ENTITY_EFFECT = 6,
    VN_MONITOR_ENTITY_CONTAINER = 7,
    VN_MONITOR_ENTITY_QUEUE = 8,
    VN_MONITOR_ENTITY_TRANSITION = 9,
    VN_MONITOR_ENTITY_TAP = 10
};

enum {
    VN_MONITOR_TAP_VOICE_PRE_CHAIN = 1,
    VN_MONITOR_TAP_VOICE_POST_CHAIN = 2,
    VN_MONITOR_TAP_EFFECT_SLOT_INPUT = 3,
    VN_MONITOR_TAP_EFFECT_SLOT_OUTPUT = 4,
    VN_MONITOR_TAP_BUS_POST_EFFECTS = 5,
    VN_MONITOR_TAP_BUS_PRE_FADER = 6,
    VN_MONITOR_TAP_BUS_POST_FADER = 7,
    VN_MONITOR_TAP_MASTER_OUTPUT = 8,
    VN_MONITOR_TAP_DEVICE_OUTPUT = 9
};

enum {
    VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE = 0,
    VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT = 1
};

enum {
    VN_MONITOR_PERM_OBSERVE_STATE = 1u << 0,
    VN_MONITOR_PERM_OBSERVE_EVENTS = 1u << 1,
    VN_MONITOR_PERM_OBSERVE_MEASUREMENTS = 1u << 2,
    VN_MONITOR_PERM_CAPTURE_CONTROL = 1u << 3,
    VN_MONITOR_PERM_RAW_AUDIO = 1u << 5,
    VN_MONITOR_PERM_SYMBOLS = 1u << 6
};

enum {
    VN_MONITOR_SECURITY_DISABLED = 0,
    VN_MONITOR_SECURITY_LOCAL_ACL = 1,
    VN_MONITOR_SECURITY_LAN_MTLS = 2
};

enum {
    VN_MONITOR_PLAN_PENDING = 0,
    VN_MONITOR_PLAN_ACTIVE = 1,
    VN_MONITOR_PLAN_RETIRED = 2,
    VN_MONITOR_PLAN_CANCELLED = 3
};

enum {
    VN_MONITOR_STATE_CUT_COMPLETE = 0,
    VN_MONITOR_STATE_CUT_TRUNCATED = 1,
    VN_MONITOR_STATE_CUT_ABORTED = 2
};

enum {
    VN_MONITOR_MEASURE_SAMPLE_PEAK = 1,
    VN_MONITOR_MEASURE_TRUE_PEAK = 2,
    VN_MONITOR_MEASURE_RMS = 3,
    VN_MONITOR_MEASURE_PEAK_HOLD = 4,
    VN_MONITOR_MEASURE_GAIN_REDUCTION = 5,
    VN_MONITOR_MEASURE_WAVEFORM = 6,
    VN_MONITOR_MEASURE_SCOPE = 7,
    VN_MONITOR_MEASURE_SPECTRUM = 8,
    VN_MONITOR_MEASURE_LUFS_MOMENTARY = 9,
    /* Deprecated. Both names retire next version. IDs 10 and 11 are permanently reserved. */
    VN_MONITOR_MEASURE_LUFS_SHORT_TERM HARKEN_DEPRECATED("Reserved measurement kind.") = 10,
    VN_MONITOR_MEASURE_LUFS_INTEGRATED HARKEN_DEPRECATED("Reserved measurement kind.") = 11
};

enum {
    VN_MONITOR_VALID_ARMED = 1u << 0,
    VN_MONITOR_VALID_NUMERIC = 1u << 1,
    VN_MONITOR_VALID_WARMUP = 1u << 2,
    VN_MONITOR_VALID_UNSUPPORTED = 1u << 3
};

enum {
    VN_MONITOR_REGISTRY_REASON = 1,
    VN_MONITOR_REGISTRY_TAP = 2,
    VN_MONITOR_REGISTRY_IDENTITY = 3,
    VN_MONITOR_REGISTRY_LINEAGE = 4,
    VN_MONITOR_REGISTRY_SYMBOLS = 5,
    VN_MONITOR_REGISTRY_CLOCK = 6,
    VN_MONITOR_REGISTRY_ALGORITHM = 7,
    VN_MONITOR_REGISTRY_CAPABILITY = 8
};

enum {
    VN_MONITOR_REMOTE_OP_DISCOVERY = 1,
    VN_MONITOR_REMOTE_OP_POLL = 2,
    VN_MONITOR_REMOTE_OP_STATE_RESYNC = 3,
    VN_MONITOR_REMOTE_OP_LEASE = 4,
    VN_MONITOR_REMOTE_OP_PLAN = 5
};

enum {
    VN_MONITOR_CHANNEL_PER_CHANNEL = 0,
    VN_MONITOR_CHANNEL_MONO_AVERAGE = 1
};

enum {
    VN_MONITOR_RECORD_FLAG_REQUIRED = 1u << 0,
    VN_MONITOR_RECORD_FLAG_OPTIONAL = 1u << 1
};

enum {
    VN_MONITOR_EVENT_REQUEST = 1,
    VN_MONITOR_EVENT_ACCEPT = 2,
    VN_MONITOR_EVENT_REJECT = 3,
    VN_MONITOR_EVENT_START = 4,
    VN_MONITOR_EVENT_STOP = 5,
    VN_MONITOR_EVENT_RESULT = 6,
    VN_MONITOR_EVENT_SELECT = 7,
    VN_MONITOR_EVENT_QUEUE = 8,
    VN_MONITOR_EVENT_TRANSITION = 9,
    VN_MONITOR_EVENT_VOICE = 10,
    VN_MONITOR_EVENT_RESOURCE = 11,
    VN_MONITOR_EVENT_PLAN = 12,
    VN_MONITOR_EVENT_DROP = 13
};

enum {
    VN_MONITOR_SCOPE_EDGE_RISING = 1,
    VN_MONITOR_SCOPE_EDGE_FALLING = 2
};

enum {
    VN_MONITOR_CHECKSUM_CRC32C = 1
};

enum {
    VN_MONITOR_REDUCTION_NONE = 0,
    VN_MONITOR_REDUCTION_PEAK = 1,
    VN_MONITOR_REDUCTION_RMS = 2
};

#define VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES 316u
#define VN_MONITOR_STATE_ENTITY_SLOTS 16u
#define VN_MONITOR_HARD_REGISTRY_BLOB_BYTES 65536u
#define VN_MONITOR_REMOTE_MESSAGE_COUNTER_START 1ull
#define VN_MONITOR_REMOTE_NONCE_BYTES 32u
#define VN_MONITOR_REMOTE_SPKI_SHA256_BYTES 32u

#define VN_MONITOR_DEFAULT_SESSIONS_PER_ENGINE 8u
#define VN_MONITOR_HARD_SESSIONS_PER_ENGINE 32u
#define VN_MONITOR_HARD_PROCESS_SESSION_SLOTS 32u
#define VN_MONITOR_DEFAULT_REMOTE_CLIENTS 4u
#define VN_MONITOR_HARD_REMOTE_CLIENTS 16u
#define VN_MONITOR_DEFAULT_PLAN_SLOTS 8u
#define VN_MONITOR_HARD_PLAN_SLOTS 32u
#define VN_MONITOR_DEDICATED_STOP_SLOTS 1u
#define VN_MONITOR_DEFAULT_PRODUCER_REGS 128u
#define VN_MONITOR_HARD_PRODUCER_REGS 512u
#define VN_MONITOR_DEFAULT_STREAMS 512u
#define VN_MONITOR_HARD_STREAMS 2048u
#define VN_MONITOR_DEFAULT_TAPS_PER_PLAN 32u
#define VN_MONITOR_HARD_TAPS_PER_PLAN 128u
#define VN_MONITOR_DEFAULT_MEASUREMENTS_PER_PLAN 128u
#define VN_MONITOR_HARD_MEASUREMENTS_PER_PLAN 512u
#define VN_MONITOR_DEFAULT_CONTROL_RECORD_BYTES 4096u
#define VN_MONITOR_HARD_CONTROL_RECORD_BYTES 16384u
#define VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES 1024u
#define VN_MONITOR_HARD_AUDIO_CHUNK_FRAMES 4096u
#define VN_MONITOR_DEFAULT_AUDIO_CHANNELS 16u
#define VN_MONITOR_HARD_AUDIO_CHANNELS 64u
#define VN_MONITOR_DEFAULT_PROCESS_QUEUE_BYTES 536870912ull
#define VN_MONITOR_HARD_PROCESS_QUEUE_BYTES 536870912ull
#define VN_MONITOR_DEFAULT_PRODUCER_RING_BYTES 4194304ull
#define VN_MONITOR_HARD_PRODUCER_RING_BYTES 67108864ull
#define VN_MONITOR_DEFAULT_CONSUMER_QUEUE_BYTES 16777216ull
#define VN_MONITOR_HARD_CONSUMER_QUEUE_BYTES 134217728ull
#define VN_MONITOR_DEFAULT_DERIVED_WORK_ITEMS 1024u
#define VN_MONITOR_HARD_DERIVED_WORK_ITEMS 8192u
#define VN_MONITOR_DEFAULT_REMOTE_QUEUE_BYTES 16777216ull
#define VN_MONITOR_HARD_REMOTE_QUEUE_BYTES 134217728ull
#define VN_MONITOR_DEFAULT_CAPTURE_BYTE_RATE 16777216ull
#define VN_MONITOR_HARD_CAPTURE_BYTE_RATE 134217728ull
#define VN_MONITOR_DEFAULT_RAW_AUDIO_BYTE_RATE 8388608ull
#define VN_MONITOR_HARD_RAW_AUDIO_BYTE_RATE 67108864ull

typedef struct VnMonitorTelemetryCorrelation {
    uint32_t structSize;
    uint32_t reserved0;
    uint64_t arrangingInstance;
    VnObjectId clipId;
    uint32_t loopIteration;
    uint32_t occurrenceOrdinal;
    uint64_t logicCommitSequence;
    uint64_t executeFrame;
} VnMonitorTelemetryCorrelation;

typedef struct VnMonitorRecordHeader {
    uint32_t structSize;
    uint32_t recordKind;
    uint32_t recordVersion;
    uint32_t flags;
    uint32_t streamKind;
    uint32_t reserved0;
    uint64_t recordBytes;
    uint64_t streamId;
    uint64_t producerId;
    uint64_t sourceSequence;
    uint64_t deliverySequence; /* consumer-local; file/Remote store 0 */
    uint64_t engineMonitorInstanceId;
    uint64_t monitorSessionEpoch;
    uint64_t worldGeneration;
    uint64_t capturePlanGeneration;
    uint64_t renderPlanGeneration;
    uint64_t transportEpoch;
    uint64_t deviceEpoch;
    uint64_t clockAnchorGeneration;
    uint64_t measurementSegmentGeneration;
    uint64_t executeFrame;
    uint64_t graphFrame;
    uint64_t devicePresentationEstimate;
    uint32_t deviceUncertaintyFrames;
    uint32_t entityKind;
    uint64_t programSemanticFingerprint;
    uint64_t runtimeLineageFingerprint;
    uint64_t optionalSymbolFingerprint;
    VnObjectId stableId;
    uint64_t instanceHandle;
    uint64_t instanceGeneration;
    uint64_t actionInstanceHandle;
    uint64_t playableInstanceHandle;
    uint64_t parentActionInstanceHandle;
    uint64_t traceCorrelationId;
    uint32_t reason;
    uint32_t validityFlags;
    uint64_t payloadBytes;
    VnMonitorTelemetryCorrelation telemetry;
} VnMonitorRecordHeader;

typedef struct VnMonitorDropSummary {
    uint32_t structSize;
    uint32_t stage;
    uint64_t streamId;
    uint64_t firstSourceSequence;
    uint64_t lastSourceSequence;
    uint64_t recordCount;
    uint64_t byteCount;
    uint32_t reason;
    uint32_t reserved0;
} VnMonitorDropSummary;

typedef struct VnMonitorStreamBaseline {
    uint32_t structSize;
    uint32_t reserved0;
    uint64_t streamId;
    uint64_t firstExpectedSourceSequence;
    uint32_t streamKind;
    uint32_t reserved1;
} VnMonitorStreamBaseline;

typedef struct VnMonitorMeasurementCapability {
    uint32_t structSize;
    uint32_t kind;
    uint32_t supported;
    uint32_t algorithmVersion;
    uint32_t unit;
    uint32_t reserved0;
    float tolerance;
    float reserved1;
} VnMonitorMeasurementCapability;

typedef struct VnMonitorCapabilities {
    uint32_t structSize;
    uint32_t reserved0;
    uint32_t monitorImplemented;
    uint32_t remoteEnabled;
    uint32_t securityProfile;
    uint32_t keepPlanAfterLeaseTimeout;
    uint32_t resolvedSessionsPerEngine;
    uint32_t hardSessionsPerEngine;
    uint32_t resolvedProcessSessionSlots;
    uint32_t hardProcessSessionSlots;
    uint64_t resolvedProcessQueueBytes;
    uint64_t hardProcessQueueBytes;
    uint32_t resolvedRemoteClients;
    uint32_t hardRemoteClients;
    uint32_t resolvedPlanSlots;
    uint32_t hardPlanSlots;
    uint32_t dedicatedStopSlots;
    uint32_t reserved1;
    uint32_t lufsMomentarySupported;
    uint32_t lufsShortTermSupported;
    uint32_t lufsIntegratedSupported;
    uint32_t reserved2;
    uint64_t capabilityFingerprint;
    uint64_t resolvedProducerRingBytes;
    uint64_t hardProducerRingBytes;
    uint64_t resolvedConsumerQueueBytes;
    uint64_t hardConsumerQueueBytes;
    uint32_t resolvedDerivedWorkItems;
    uint32_t hardDerivedWorkItems;
    uint64_t resolvedRemoteQueueBytes;
    uint64_t hardRemoteQueueBytes;
    uint64_t resolvedCaptureByteRate;
    uint64_t hardCaptureByteRate;
    uint64_t resolvedRawAudioByteRate;
    uint64_t hardRawAudioByteRate;
} VnMonitorCapabilities;

typedef struct VnMonitorSessionDesc {
    uint32_t structSize;
    uint32_t requestedStreamKindMask;
    uint32_t requestRawAudio;
    uint32_t requestSymbols;
    uint64_t consumerQueueBytes;
    uint32_t requestedPermissionMask;
    uint32_t reserved0;
} VnMonitorSessionDesc;

/* Execution records use independent field validity and the existing envelope. */

enum {
    VN_MONITOR_FACT_UNKNOWN = 0,
    VN_MONITOR_FACT_REQUEST_SUBMITTED = 1,
    VN_MONITOR_FACT_REQUEST_RETURNED = 2,
    VN_MONITOR_FACT_REQUEST_TERMINAL = 3,
    VN_MONITOR_FACT_PLAYABLE_PREPARED = 4,
    VN_MONITOR_FACT_PLAYABLE_ACTIVE = 5,
    VN_MONITOR_FACT_PLAYABLE_LOGICAL_COMPLETE = 6,
    VN_MONITOR_FACT_PLAYABLE_AUDIBLE_RETIRING = 7,
    VN_MONITOR_FACT_PLAYABLE_RETIRED = 8,
    VN_MONITOR_FACT_PLAYABLE_CANCELED = 9,
    VN_MONITOR_FACT_ACTION_ACCEPTED = 10,
    VN_MONITOR_FACT_ACTION_STOPPED = 11,
    VN_MONITOR_FACT_ACTION_RESULT = 12,
    VN_MONITOR_FACT_ACTION_REJECTED = 13,
    VN_MONITOR_FACT_QUEUE_ADVANCED = 14,
    VN_MONITOR_FACT_QUEUE_FAILED = 15,
    VN_MONITOR_FACT_TRANSITION_STARTED = 16,
    VN_MONITOR_FACT_TRANSITION_STOPPED = 17,
    VN_MONITOR_FACT_TRANSITION_REJECTED = 18,
    VN_MONITOR_FACT_VOICE_PENDING = 19,
    VN_MONITOR_FACT_VOICE_LIVE = 20,
    VN_MONITOR_FACT_VOICE_PARKED = 21,
    VN_MONITOR_FACT_VOICE_ENDED = 22,
    VN_MONITOR_FACT_SELECTION_COMMITTED = 23,
    VN_MONITOR_FACT_EMITTER_CREATED = 24,
    VN_MONITOR_FACT_EMITTER_DESTROYED = 25,
    VN_MONITOR_FACT_EMITTER_BINDING_CHANGED = 26,
    VN_MONITOR_FACT_HOST_REGISTERED = 27,
    VN_MONITOR_FACT_HOST_UNREGISTERED = 28,
    VN_MONITOR_FACT_IDENTITY_UPSERT = 29,
    VN_MONITOR_FACT_IDENTITY_TOMBSTONE = 30,
    VN_MONITOR_FACT_RENDER_STARTED = 31,
    VN_MONITOR_FACT_SOURCE_ENDED = 32,
    VN_MONITOR_FACT_RENDER_ACTIVATION_ENDED = 33,
    VN_MONITOR_FACT_LOGICAL_VOICE_TERMINAL = 34,
    VN_MONITOR_FACT_ACTION_TERMINAL = 35,
    VN_MONITOR_FACT_WAITING = 36,
    VN_MONITOR_FACT_SOURCE_APPLIED = 37,
    VN_MONITOR_FACT_SEEK_APPLIED = 38,
    VN_MONITOR_FACT_RESOURCE_RETIRED = 39,
    VN_MONITOR_FACT_PLAYABLE_STARTED = 40,
    VN_MONITOR_FACT_STEP_EXECUTED = 41,
    VN_MONITOR_FACT_VARIABLE_RESULT = 42,
    VN_MONITOR_FACT_PLAYABLE_FINISHED = 43,
    VN_MONITOR_FACT_STREAM_OBSERVATION = 44
};

enum {
    VN_MONITOR_FACT_FIELD_SUBJECT = 0,
    VN_MONITOR_FACT_FIELD_PROJECT = 1,
    VN_MONITOR_FACT_FIELD_CONTENT_OBJECT = 2,
    VN_MONITOR_FACT_FIELD_CONTENT_WORLD = 3,
    VN_MONITOR_FACT_FIELD_CONTENT_SEMANTIC = 4,
    VN_MONITOR_FACT_FIELD_CONTENT_LINEAGE = 5,
    VN_MONITOR_FACT_FIELD_CONTENT_REVISION = 6,
    VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST = 7,
    VN_MONITOR_FACT_FIELD_PLAYBACK_OPERATION = 8,
    VN_MONITOR_FACT_FIELD_PLAYBACK_ORIGIN = 9,
    VN_MONITOR_FACT_FIELD_CONTROL_REQUEST = 10,
    VN_MONITOR_FACT_FIELD_CONTROL_OPERATION = 11,
    VN_MONITOR_FACT_FIELD_CONTROL_ORIGIN = 12,
    VN_MONITOR_FACT_FIELD_CONTROL_ACTION = 13,
    VN_MONITOR_FACT_FIELD_CONTROL_PARENT = 14,
    VN_MONITOR_FACT_FIELD_CONTROL_ORDINAL = 15,
    VN_MONITOR_FACT_FIELD_SELECTION_REQUEST = 16,
    VN_MONITOR_FACT_FIELD_SELECTION_OPERATION = 17,
    VN_MONITOR_FACT_FIELD_SELECTION_ORIGIN = 18,
    VN_MONITOR_FACT_FIELD_SELECTION_ACTION = 19,
    VN_MONITOR_FACT_FIELD_SELECTION_PARENT = 20,
    VN_MONITOR_FACT_FIELD_SELECTION_ORDINAL = 21,
    VN_MONITOR_FACT_FIELD_PARENT_PLAYABLE = 22,
    VN_MONITOR_FACT_FIELD_PARENT_ACTION = 23,
    VN_MONITOR_FACT_FIELD_ROOT = 24,
    VN_MONITOR_FACT_FIELD_RELATED_ACTION = 25,
    VN_MONITOR_FACT_FIELD_SERVICE_LEAF = 26,
    VN_MONITOR_FACT_FIELD_RESOURCE_OWNER = 27,
    VN_MONITOR_FACT_FIELD_LOGICAL_VOICE = 28,
    VN_MONITOR_FACT_FIELD_RENDER_ACTIVATION = 29,
    VN_MONITOR_FACT_FIELD_ORIGIN_EMITTER = 30,
    VN_MONITOR_FACT_FIELD_ORIGIN_KIND = 31,
    VN_MONITOR_FACT_FIELD_HOST_KEY = 32,
    VN_MONITOR_FACT_FIELD_HOST_REVISION = 33,
    VN_MONITOR_FACT_FIELD_HOST_OWNERSHIP = 34,
    VN_MONITOR_FACT_FIELD_CURRENT_STATE_EMITTER = 35,
    VN_MONITOR_FACT_FIELD_CURRENT_SPATIAL_EMITTER = 36,
    VN_MONITOR_FACT_FIELD_STATE_BINDING = 37,
    VN_MONITOR_FACT_FIELD_SPATIAL_BINDING = 38,
    VN_MONITOR_FACT_FIELD_LOGIC_COMMIT = 39,
    VN_MONITOR_FACT_FIELD_READ_CUT_KEY = 40,
    VN_MONITOR_FACT_FIELD_READ_CUT_SEQUENCE = 41,
    VN_MONITOR_FACT_FIELD_READ_CUT_ORDINAL = 42,
    VN_MONITOR_FACT_FIELD_EXECUTE_FRAME = 43,
    VN_MONITOR_FACT_FIELD_GRAPH_FRAME = 44,
    VN_MONITOR_FACT_FIELD_TRANSPORT = 45,
    VN_MONITOR_FACT_FIELD_AUDIO_BLOCK = 46,
    VN_MONITOR_FACT_FIELD_FRAME_OFFSET = 47,
    VN_MONITOR_FACT_FIELD_FRAME_SPAN = 48,
    VN_MONITOR_FACT_FIELD_ARRANGING = 49,
    VN_MONITOR_FACT_FIELD_CLIP = 50,
    VN_MONITOR_FACT_FIELD_LOOP = 51,
    VN_MONITOR_FACT_FIELD_OCCURRENCE = 52,
    VN_MONITOR_FACT_FIELD_OWNER_EPOCH = 53,
    VN_MONITOR_FACT_FIELD_RESULT = 54,
    VN_MONITOR_FACT_FIELD_REASON = 55,
    VN_MONITOR_FACT_FIELD_TERMINAL_KIND = 56,
    VN_MONITOR_FACT_FIELD_TIMESTAMP = 57,
    VN_MONITOR_FACT_FIELD_REQUEST = 58,
    VN_MONITOR_FACT_FIELD_REQUEST_OPERATION = 59,
    VN_MONITOR_FACT_FIELD_REQUEST_ORIGIN = 60,
    VN_MONITOR_FACT_FIELD_CAUSED_BY_REQUEST = 61,
    VN_MONITOR_FACT_FIELD_CAUSED_BY_OPERATION = 62,
    VN_MONITOR_FACT_FIELD_CAUSED_BY_ORIGIN = 63,
    VN_MONITOR_FACT_FIELD_TARGET_HANDLE = 64,
    VN_MONITOR_FACT_FIELD_TARGET_VALIDATED = 65,
    VN_MONITOR_FACT_FIELD_TARGET_PROJECT = 66,
    VN_MONITOR_FACT_FIELD_TARGET_OBJECT = 67,
    VN_MONITOR_FACT_FIELD_TARGET_WORLD = 68,
    VN_MONITOR_FACT_FIELD_TARGET_SEMANTIC = 69,
    VN_MONITOR_FACT_FIELD_TARGET_REVISION = 70,
    VN_MONITOR_FACT_FIELD_TARGET_KIND = 71,
    VN_MONITOR_FACT_FIELD_TARGET_CONTENT_KIND = 72,
    VN_MONITOR_FACT_FIELD_TARGET_SCOPE = 73,
    VN_MONITOR_FACT_FIELD_ACTIVITY_STEP = 74,
    VN_MONITOR_FACT_FIELD_ACTIVITY_VARIABLE = 75,
    VN_MONITOR_FACT_FIELD_ACTION_OUTCOME = 76
};

enum {
    VN_MONITOR_SUBJECT_NONE = 0,
    VN_MONITOR_SUBJECT_PLAYABLE = 1,
    VN_MONITOR_SUBJECT_ACTION = 2,
    VN_MONITOR_SUBJECT_LOGICAL_VOICE = 3,
    VN_MONITOR_SUBJECT_RENDER_ACTIVATION = 4,
    VN_MONITOR_SUBJECT_EMITTER = 5,
    VN_MONITOR_SUBJECT_QUEUE = 6,
    VN_MONITOR_SUBJECT_TRANSITION = 7,
    VN_MONITOR_SUBJECT_HOST_SYMBOL = 8
};

enum {
    VN_MONITOR_CONTENT_UNKNOWN = 0,
    VN_MONITOR_CONTENT_SOUND = 1,
    VN_MONITOR_CONTENT_CONTAINER = 2,
    VN_MONITOR_CONTENT_TRACK = 3,
    VN_MONITOR_CONTENT_SEGMENT = 4,
    VN_MONITOR_CONTENT_ACTION = 5,
    VN_MONITOR_CONTENT_MEDIA = 6,
    VN_MONITOR_CONTENT_VARIABLE = 7
};

enum {
    VN_MONITOR_TERMINAL_NATURAL_COMPLETION = 1,
    VN_MONITOR_TERMINAL_REQUESTED_STOP = 2,
    VN_MONITOR_TERMINAL_PARENT_CANCELLATION = 3,
    VN_MONITOR_TERMINAL_REPLACEMENT_RESET = 4,
    VN_MONITOR_TERMINAL_FAILURE = 5
};

enum {
    VN_MONITOR_TARGET_NONE = 0,
    VN_MONITOR_TARGET_PLAYABLE = 1,
    VN_MONITOR_TARGET_ACTION = 2,
    VN_MONITOR_TARGET_LOGICAL_VOICE = 3,
    VN_MONITOR_TARGET_RENDER_ACTIVATION = 4,
    VN_MONITOR_TARGET_EMITTER = 5,
    VN_MONITOR_TARGET_PREVIEW_PLAYABLE = 6,
    VN_MONITOR_TARGET_CONTENT = 7
};

enum {
    VN_MONITOR_FACT_FLAG_IDENTITY_INCOMPLETE = 1,
    VN_MONITOR_FACT_FLAG_CONTROL_INCOMPLETE = 2
};

typedef struct VnMonitorFactRequest {
    uint64_t sequence;
    uint32_t operation;
    uint32_t origin;
} VnMonitorFactRequest;

typedef struct VnMonitorFactCause {
    uint64_t action;
    uint64_t parentPlayable;
    uint32_t executionOrdinal;
    uint32_t reserved0;
} VnMonitorFactCause;

typedef struct VnMonitorFactTarget {
    uint64_t handle;
    VnObjectId projectId;
    VnObjectId objectId;
    uint64_t worldGeneration;
    uint64_t programSemanticFingerprint;
    uint64_t contentRevision;
    uint32_t kind;
    uint32_t contentKind;
    uint32_t scope;
    uint32_t reserved0;
} VnMonitorFactTarget;

typedef struct VnMonitorExecutionFactV1 {
    uint32_t structSize;
    uint16_t factKind;
    uint8_t subjectNamespace;
    uint8_t contentKind;
    uint64_t validityLo;
    uint64_t validityHi;
    uint64_t timestampNs;
    VnObjectId projectId;
    uint64_t contentRevision;
    uint32_t factFlags;
    uint32_t reserved0;
    VnMonitorFactRequest request;
    VnMonitorFactRequest playbackRequest;
    VnMonitorFactRequest causedByRequest;
    VnMonitorFactRequest controlRequest;
    VnMonitorFactRequest selectionRequest;
    VnMonitorFactCause control;
    VnMonitorFactCause selection;
    uint64_t root;
    uint64_t serviceLeaf;
    uint64_t resourceOwner;
    uint64_t logicalVoice;
    uint64_t renderActivation;
    uint64_t originEmitter;
    uint64_t hostSession;
    uint64_t hostLocalSequence;
    uint64_t originHostRevision;
    uint64_t currentStateEmitter;
    uint64_t currentSpatialEmitter;
    uint32_t originKind;
    uint32_t hostOwnership;
    uint32_t stateBinding;
    uint32_t spatialBinding;
    uint32_t terminalKind;
    int32_t result;
    uint64_t readCutKey;
    uint64_t readCutSequence;
    uint64_t audioBlock;
    uint64_t ownerEpoch;
    uint32_t frameOffset;
    uint32_t frameSpan;
    uint32_t readCutOrdinalExclusive;
    uint32_t reserved1;
    VnMonitorFactTarget target;
} VnMonitorExecutionFactV1;

typedef struct VnMonitorFactActivity {
    uint32_t actionOutcome;
    uint32_t stepIndex;
    uint32_t stepExecutionOrdinal;
    uint32_t stepOpcode;
    uint32_t reservedStep;
    uint32_t stepTargetKind;
    uint32_t variableValueBits;
    uint32_t variableScope;
    uint32_t variableOutcome;
    uint32_t reserved0;
    VnObjectId stepTargetId;
    uint64_t variableTicket;
} VnMonitorFactActivity;

typedef struct VnMonitorExecutionFactV2 {
    VnMonitorExecutionFactV1 fact;
    VnMonitorFactActivity activity;
} VnMonitorExecutionFactV2;

#define VN_MONITOR_PLAYABLE_PROVENANCE_KNOWN 1

/* Exact V2 prefix. Known provenance identifies a resolved single-source occurrence. */
typedef struct VnMonitorPlayableProvenance {
    VnObjectId rootPlayableId;
    uint64_t rootProgramFingerprint;
    uint64_t occurrenceFingerprint;
    uint64_t mediaChecksum;
    uint32_t childOrdinal;
    uint32_t knownFlags;
} VnMonitorPlayableProvenance;

typedef struct VnMonitorExecutionFactV3 {
    VnMonitorExecutionFactV1 fact;
    VnMonitorFactActivity activity;
    VnMonitorPlayableProvenance playable;
} VnMonitorExecutionFactV3;

#define VN_MONITOR_STREAM_FACT_SUPPLY 1
#define VN_MONITOR_STREAM_FACT_AUDIO 2

/* readStatus: 0 cache/no call, 1 success, 2 I/O/short read, 3 corrupt
 * content, 4 provider exception, 5 cancellation. TerminalReason retains the
 * playback contract: 0 none, 1 natural EOF, 2 integrity abort, 3 producer failure. */

/* EXECUTION_FACT recordVersion=4, enabled by requestedExecutionFactVersion=4.
 * Other execution facts retain their V1/V2/V3 payloads. The envelope carries
 * the existing engine/session/source sequence and playback associations.
 * Supply records coalesce completed jobs; readSequence and read timestamps
 * identify the last job, while totals cover the whole ring generation. The
 * byte range is the LAST actual provider call (not a contiguous union of
 * planar reads); readCalls/readBytes/readCostNs describe the complete job.
 * Cache-only verification has zero readCalls and cost. Cost is elapsed provider
 * time, including waits, not CPU usage. Source frames name the job's source chunk.
 * Audio records describe exactly [frameBegin,frameEnd) of audioBlock. Their
 * supply-only fields are zero; supply records never invent an audio block.
 * Both sides use the same sourceGeneration/ringToken/renderActivation keys.
 * Observation does not retain a source or alter playback/recovery. */
typedef struct VnMonitorStreamFactV1 {
    uint32_t structSize;
    uint32_t factKind;
    uint64_t sourceGeneration;
    uint64_t contentRevision;
    uint64_t ringToken;
    uint64_t preparationGeneration;
    uint64_t renderActivation;
    uint64_t logicalVoice;
    uint64_t readSequence;
    uint64_t sourceFrameBegin;
    uint64_t sourceFrameEnd;
    uint64_t readOffset;
    uint64_t readRequestedBytes;
    uint64_t lastReadBytes;
    uint64_t readCalls;
    uint64_t readBytes;
    uint64_t readCostNs;
    uint64_t totalReadCalls;
    uint64_t totalReadBytes;
    uint64_t totalReadCostNs;
    uint64_t ringReadIndex;
    uint64_t ringWriteIndex;
    uint64_t readBeginNs;
    uint64_t readEndNs;
    uint64_t audioBlock;
    uint64_t frameBegin;
    uint64_t frameEnd;
    uint64_t appliedFrame;
    uint64_t replacementSerial;
    uint64_t actionFiring;
    uint64_t playableInstance;
    uint64_t worldGeneration;
    uint64_t transportGeneration;
    uint32_t bufferedFrames;
    uint32_t prebufferTargetFrames;
    uint32_t state;
    uint32_t terminalReason;
    VnResult result;
    uint32_t flags;
    uint32_t producedFrames;
    uint32_t starved;
    uint32_t readStatus;
    uint32_t reserved0;
} VnMonitorStreamFactV1;

/* STATE identity revision is independent of native ReadCut and EVENT sequences. */
typedef struct VnMonitorIdentityFactV1 {
    uint32_t structSize;
    uint32_t reserved0;
    uint64_t stateCutId;
    uint64_t identityRevision;
    VnMonitorExecutionFactV1 fact;
} VnMonitorIdentityFactV1;

enum {
    VN_MONITOR_HOST_SYMBOL_NAME = 1,
    VN_MONITOR_HOST_SYMBOL_PATH = 2,
    VN_MONITOR_HOST_SYMBOL_SCENE = 3
};

enum {
    VN_MONITOR_SYMBOL_UPSERT = 1,
    VN_MONITOR_SYMBOL_TOMBSTONE = 2
};

/* Emitter and engine identities live in the envelope. Empty text is a value. */
typedef struct VnMonitorHostObjectSymbolV1 {
    uint32_t structSize;
    uint32_t factKind;
    uint64_t stateCutId;
    uint64_t cutRevision;
    uint64_t hostSession;
    uint64_t hostSequence;
    uint64_t registrationRevision;
    uint32_t field;
    uint16_t textBytes;
    uint16_t flags;
    uint8_t text[256];
} VnMonitorHostObjectSymbolV1;

/* HOST_SYMBOL counts individual fields, including valid empty fields. */
typedef struct VnMonitorIdentityCutBeginV1 {
    uint32_t structSize;
    uint32_t reserved0;
    uint64_t stateCutId;
    uint64_t identityRevision;
    uint64_t deltaProducerId;
    uint64_t deltaSourceSequence;
    uint64_t symbolProducerId;
    uint64_t symbolSourceSequence;
    uint64_t capacityLimit;
    uint64_t namespaceTrueCount[9];
} VnMonitorIdentityCutBeginV1;

typedef struct VnMonitorIdentityCutEndV1 {
    uint32_t structSize;
    uint32_t result;
    uint64_t stateCutId;
    uint64_t identityRevision;
    uint64_t checksum;
    uint64_t namespaceEmittedCount[9];
} VnMonitorIdentityCutEndV1;

/* Exact legacy prefix; extensions require the full declared V2 size. */
typedef struct VnMonitorSessionDescV2 {
    uint32_t structSize;
    uint32_t requestedStreamKindMask;
    uint32_t requestRawAudio;
    uint32_t requestSymbols;
    uint64_t consumerQueueBytes;
    uint32_t requestedPermissionMask;
    uint32_t reserved0;
    uint32_t requestedExecutionFactVersion;
    uint32_t reserved1;
} VnMonitorSessionDescV2;

typedef struct VnMonitorSubscriptionStatus {
    uint32_t structSize;
    uint32_t requestedStreamKindMask;
    uint32_t resolvedStreamKindMask;
    uint32_t subscriptionGeneration;
    uint32_t requestedPermissionMask;
    uint32_t acceptedPermissionMask;
    uint32_t deniedPermissionMask;
    uint32_t unsupportedPermissionMask;
    uint64_t monitorSessionEpoch;
    uint32_t rawAudioDecision;
    uint32_t symbolsDecision;
    uint32_t baselineCapacity;
    uint32_t baselineCount;
    VnMonitorStreamBaseline* baselines;
} VnMonitorSubscriptionStatus;

/* Exact legacy prefix with execution observation negotiation. */
typedef struct VnMonitorSubscriptionStatusV2 {
    uint32_t structSize;
    uint32_t requestedStreamKindMask;
    uint32_t resolvedStreamKindMask;
    uint32_t subscriptionGeneration;
    uint32_t requestedPermissionMask;
    uint32_t acceptedPermissionMask;
    uint32_t deniedPermissionMask;
    uint32_t unsupportedPermissionMask;
    uint64_t monitorSessionEpoch;
    uint32_t rawAudioDecision;
    uint32_t symbolsDecision;
    uint32_t baselineCapacity;
    uint32_t baselineCount;
    VnMonitorStreamBaseline* baselines;
    uint32_t acceptedExecutionFactVersion;
    uint32_t baselineAvailability;
    uint64_t supportedFactKinds[2];
    uint32_t symbolAvailability;
    uint32_t reserved0;
    uint64_t identityUnavailableCount;
    uint64_t baselineStateCutId;
} VnMonitorSubscriptionStatusV2;

typedef struct VnMonitorStateCutDesc {
    uint32_t structSize;
    uint32_t reserved0;
    uint64_t requestedCapacityHint;
} VnMonitorStateCutDesc;

typedef struct VnMonitorStateCutStatus {
    uint32_t structSize;
    uint32_t result;
    uint64_t stateCutId;
    uint64_t worldGeneration;
    uint64_t capturePlanGeneration;
    uint32_t emittedCount;
    uint32_t reserved0;
    uint64_t checksum;
} VnMonitorStateCutStatus;

typedef struct VnMonitorCaptureControlStatus {
    uint32_t structSize;
    uint32_t reserved0;
    uint64_t leaseGeneration;
    uint64_t principalHash;
    uint32_t permissions;
    uint32_t held;
    uint64_t expiryExecuteFrame;
    uint32_t lastChangeReason;
    uint32_t reserved1;
} VnMonitorCaptureControlStatus;

typedef struct VnMonitorTapDescriptor VnMonitorTapDescriptor;
typedef struct VnMonitorMeasurementRequest VnMonitorMeasurementRequest;

typedef struct VnMonitorCapturePlan {
    uint32_t structSize;
    uint32_t streamKindMask;
    uint32_t tapCount;
    uint32_t measurementCount;
    uint32_t cadenceHz;
    uint32_t downsample;
    uint32_t channelMode;
    uint32_t reserved0;
    uint64_t bytesPerSecondBudget;
    uint64_t sessionByteBudget;
    uint64_t maxDurationExecuteFrames;
    uint32_t remotePermissionMask;
    uint32_t reserved1;
    const VnMonitorTapDescriptor* taps;
    const VnMonitorMeasurementRequest* measurements;
} VnMonitorCapturePlan;

typedef struct VnMonitorEffectiveCapturePlan {
    uint32_t structSize;
    uint32_t activationState;
    uint64_t capturePlanGeneration;
    uint64_t previousGeneration;
    uint64_t effectiveFromExecuteFrame;
    VnMonitorCapturePlan accepted;
} VnMonitorEffectiveCapturePlan;

typedef struct VnMonitorPollDesc {
    uint32_t structSize;
    uint32_t reserved0;
    void* buffer;
    uint64_t bufferBytes;
} VnMonitorPollDesc;

typedef struct VnMonitorPollResult {
    uint32_t structSize;
    uint32_t recordsWritten;
    uint64_t bytesWritten;
    uint64_t nextRecordBytes;
    uint64_t recordsRemaining;
    uint64_t firstDeliverySequence;
    uint64_t lastDeliverySequence;
} VnMonitorPollResult;


typedef struct VnMonitorTapDescriptor {
    uint32_t structSize;
    uint32_t tapKind;
    uint64_t tapRegistryGeneration;
    uint64_t tapDescriptorId;
    uint64_t tapDescriptorGeneration;
    uint32_t ownerKind;
    uint32_t selectorMode;
    VnObjectId ownerStableId;
    uint64_t ownerInstanceHandle;
    uint64_t ownerGeneration;
    uint32_t slotId;
    uint32_t slotGeneration;
    uint64_t expectedEffectGeneration;
    uint32_t layout;
    uint32_t sampleRate;
    uint32_t channelOrder;
    uint32_t capabilityState;
    uint32_t availabilityState;
    uint32_t reason;
} VnMonitorTapDescriptor;

typedef struct VnMonitorMeasurementRequest {
    uint32_t structSize;
    uint32_t kind;
    uint32_t tapIndex;
    uint32_t channelMode;
    uint32_t downsample;
    uint32_t cadenceHz;
    uint32_t bucketFrames;
    uint32_t fftSize;
    uint32_t windowKind;
    uint32_t overlapNumerator;
    uint32_t overlapDenominator;
    uint32_t reserved0;
    uint64_t tapDescriptorId;
    float triggerThreshold;
    float hysteresis;
    uint32_t triggerMode;
    uint32_t reserved1;
} VnMonitorMeasurementRequest;

typedef struct VnMonitorStateCutBegin {
    uint32_t structSize;
    uint32_t reserved0;
    uint64_t stateCutId;
    uint64_t worldGeneration;
    uint64_t capturePlanGeneration;
    uint64_t renderPlanGeneration;
    uint64_t transportGeneration;
    uint64_t capacityLimit;
    uint64_t entityTrueCount[16];
} VnMonitorStateCutBegin;

typedef struct VnMonitorStateUpsert {
    uint32_t structSize;
    uint32_t entityKind;
    uint64_t stateCutId;
    VnObjectId stableId;
    uint64_t instanceHandle;
    uint64_t instanceGeneration;
    float volumeDb;
    uint32_t mute01;
    uint32_t solo01;
    uint32_t bypass01;
    uint32_t latencyFrames;
    uint32_t tailFrames;
    VnObjectId routeBusId;
    uint32_t sendCount;
    uint32_t occupancy;
    uint32_t highWater;
    uint32_t rejectCount;
    uint32_t dropCount;
    uint32_t reserved0;
} VnMonitorStateUpsert;

typedef struct VnMonitorStateTombstone {
    uint32_t structSize;
    uint32_t entityKind;
    uint64_t stateCutId;
    VnObjectId stableId;
    uint64_t instanceHandle;
    uint64_t instanceGeneration;
    uint32_t reason;
    uint32_t reserved0;
} VnMonitorStateTombstone;

typedef struct VnMonitorStateCutEnd {
    uint32_t structSize;
    uint32_t result;
    uint64_t stateCutId;
    uint32_t checksumAlgorithm;
    uint32_t reserved0;
    uint64_t checksum;
    uint64_t emittedCount[16];
} VnMonitorStateCutEnd;

typedef struct VnMonitorEventPayload {
    uint32_t structSize;
    uint32_t eventClass;
    uint32_t reason;
    uint32_t reserved0;
    uint64_t playableInstanceHandle;
    uint64_t actionInstanceHandle;
    uint64_t voiceInstanceHandle;
    uint64_t parentActionInstanceHandle;
} VnMonitorEventPayload;

enum {
    VN_MONITOR_EVENT_CATEGORY_OPERATION = 1,
    VN_MONITOR_EVENT_CATEGORY_LIFECYCLE = 2,
    VN_MONITOR_EVENT_CATEGORY_DIAGNOSTIC = 3,
    VN_MONITOR_EVENT_SESSION_LIVE = 0,
    VN_MONITOR_EVENT_SESSION_SEALED = 1,
    VN_MONITOR_EVENT_SESSION_DRAINED = 2
};

enum {
    VN_MONITOR_OPERATION_POST_ACTION = 1,
    VN_MONITOR_OPERATION_POST_ACTION_BY_NAME = 2,
    VN_MONITOR_OPERATION_PLAY_PREVIEW = 3,
    VN_MONITOR_OPERATION_STOP_PREVIEW = 4,
    VN_MONITOR_OPERATION_STOP_VOICE = 5,
    VN_MONITOR_OPERATION_STOP_ACTION = 6,
    VN_MONITOR_OPERATION_STOP_ALL = 7,
    VN_MONITOR_OPERATION_SET_VALUE = 8,
    VN_MONITOR_OPERATION_SET_EMITTER_VALUE = 9,
    VN_MONITOR_OPERATION_LOAD_PACK = 10,
    VN_MONITOR_OPERATION_UNLOAD_PACK = 11,
    VN_MONITOR_OPERATION_SET_VALUES = 12,
    VN_MONITOR_OPERATION_SET_EMITTER_VALUES = 13,
    VN_MONITOR_OPERATION_POST_ACTION_PINNED = 14,
    VN_MONITOR_OPERATION_POST_ACTION_RESOLVED = 15,
    VN_MONITOR_OPERATION_STOP_EMITTER = 16,
    VN_MONITOR_OPERATION_RESET_VALUES = 19
};

enum {
    VN_MONITOR_LIFECYCLE_PLAYABLE_START = 1,
    VN_MONITOR_LIFECYCLE_PLAYABLE_LOGICAL_COMPLETE = 2,
    VN_MONITOR_LIFECYCLE_PLAYABLE_AUDIBLE_RETIRING = 3,
    VN_MONITOR_LIFECYCLE_PLAYABLE_RETIRED = 4,
    VN_MONITOR_LIFECYCLE_PLAYABLE_CANCEL = 5,
    VN_MONITOR_LIFECYCLE_ACTION_ACCEPT = 6,
    VN_MONITOR_LIFECYCLE_ACTION_STOP = 7,
    VN_MONITOR_LIFECYCLE_ACTION_RESULT = 8,
    VN_MONITOR_LIFECYCLE_ACTION_REJECT = 9,
    VN_MONITOR_LIFECYCLE_QUEUE_ADVANCE = 10,
    VN_MONITOR_LIFECYCLE_QUEUE_FAIL = 11,
    VN_MONITOR_LIFECYCLE_TRANSITION_START = 12,
    VN_MONITOR_LIFECYCLE_TRANSITION_STOP = 13,
    VN_MONITOR_LIFECYCLE_TRANSITION_REJECT = 14,
    VN_MONITOR_LIFECYCLE_VOICE_PENDING = 15,
    VN_MONITOR_LIFECYCLE_VOICE_LIVE = 16,
    VN_MONITOR_LIFECYCLE_VOICE_PARK = 17,
    VN_MONITOR_LIFECYCLE_VOICE_END = 18,
    VN_MONITOR_LIFECYCLE_EFFECT_INSTALLED = 19,
    VN_MONITOR_LIFECYCLE_EFFECT_RELEASED = 20
};

typedef struct VnMonitorEventDetail {
    uint32_t structSize;
    uint32_t category;
    uint32_t code;
    uint32_t level;
    int32_t result;
    uint32_t reserved0;
    uint64_t timestampNs;
    uint64_t arg0;
    uint64_t arg1;
    uint32_t valueBits;
    uint32_t textBytes;
    char text[192];
} VnMonitorEventDetail;

typedef struct VnMonitorEventDetailV2 {
    uint32_t structSize;
    uint32_t category;
    uint32_t code;
    uint32_t level;
    int32_t result;
    uint32_t reserved0;
    uint64_t timestampNs;
    uint64_t arg0;
    uint64_t arg1;
    uint32_t valueBits;
    uint32_t textBytes;
    char text[192];
    uint8_t projectGuid[16];
} VnMonitorEventDetailV2;

typedef struct VnMonitorEventSessionStatus {
    uint32_t structSize;
    uint32_t state;
    uint64_t monitorSessionEpoch;
    uint64_t startedTimestampNs;
    uint64_t stoppedTimestampNs;
    uint64_t sourceDroppedRecords;
    uint64_t deliveryDroppedRecords;
} VnMonitorEventSessionStatus;

enum {
    VN_MONITOR_OBSERVATION_NOT_REQUESTED = 0,
    VN_MONITOR_OBSERVATION_AVAILABLE = 1,
    VN_MONITOR_OBSERVATION_PENDING = 2,
    VN_MONITOR_OBSERVATION_DENIED = 3,
    VN_MONITOR_OBSERVATION_UNSUPPORTED = 4,
    VN_MONITOR_OBSERVATION_INCOMPLETE = 5
};

/* Exact legacy prefix; extensions require the full declared V2 size. */
typedef struct VnMonitorEventSessionStatusV2 {
    uint32_t structSize;
    uint32_t state;
    uint64_t monitorSessionEpoch;
    uint64_t startedTimestampNs;
    uint64_t stoppedTimestampNs;
    uint64_t sourceDroppedRecords;
    uint64_t deliveryDroppedRecords;
    uint32_t acceptedExecutionFactVersion;
    uint32_t baselineAvailability;
    uint64_t supportedFactKinds[2];
    uint32_t requestedPermissionMask;
    uint32_t acceptedPermissionMask;
    uint32_t deniedPermissionMask;
    uint32_t unsupportedPermissionMask;
    uint32_t acceptedStreamKindMask;
    uint32_t symbolsDecision;
    uint32_t symbolAvailability;
    uint32_t reserved0;
    uint64_t identityUnavailableCount;
    uint64_t baselineStateCutId;
} VnMonitorEventSessionStatusV2;

typedef struct VnMonitorTimeSeriesSample {
    uint32_t structSize;
    uint32_t metricId;
    uint32_t cadenceHz;
    uint32_t unitId;
    uint32_t algorithmId;
    uint32_t algorithmVersion;
    uint32_t valueBits;
    uint32_t toleranceBits;
} VnMonitorTimeSeriesSample;

typedef struct VnMonitorAudioChunk {
    uint32_t structSize;
    uint32_t sampleFormat;
    uint64_t sourceFrameBegin;
    uint64_t sourceFrameEnd;
    uint64_t sourceSequenceFirst;
    uint64_t sourceSequenceLast;
    uint32_t sampleRate;
    uint32_t channelCount;
    uint32_t channelOrder;
    uint32_t layout;
    uint64_t tapDescriptorId;
    uint64_t tapDescriptorGeneration;
    uint32_t algorithmId;
    uint32_t algorithmVersion;
} VnMonitorAudioChunk;

typedef struct VnMonitorWaveformBucket {
    uint32_t structSize;
    uint32_t bucketFrames;
    uint32_t channelMode;
    uint32_t reductionMethod;
    uint64_t sourceFrameOrigin;
    uint64_t sourceFrameBegin;
    uint64_t sourceFrameEnd;
    uint64_t sourceSequenceFirst;
    uint64_t sourceSequenceLast;
    uint32_t algorithmId;
    uint32_t algorithmVersion;
    uint32_t unitId;
    uint32_t toleranceBits;
    uint32_t partialDiscard;
    uint32_t reserved0;
} VnMonitorWaveformBucket;

typedef struct VnMonitorScopeResult {
    uint32_t structSize;
    uint32_t edge;
    uint32_t channelMode;
    uint32_t channelIndex;
    uint32_t preTriggerFrames;
    uint32_t postTriggerFrames;
    uint32_t maxSearchFrames;
    uint32_t thresholdBits;
    uint32_t hysteresisBits;
    uint32_t executedPreTriggerFrames;
    uint32_t executedPostTriggerFrames;
    uint32_t executedMaxSearchFrames;
    uint32_t executedEdge;
    uint32_t executedThresholdBits;
    uint32_t executedHysteresisBits;
    uint32_t executedChannelMode;
    uint32_t noTrigger;
    uint32_t discontinuityFlags;
    uint64_t triggerFrame;
    uint64_t windowBegin;
    uint64_t windowEnd;
    uint64_t engineMonitorInstanceId;
    uint64_t streamId;
    uint64_t measurementSegmentGeneration;
    uint64_t triggerSourceSequence;
    uint64_t capturePlanGeneration;
    uint64_t tapDescriptorGeneration;
} VnMonitorScopeResult;

typedef struct VnMonitorSpectrumPayload {
    uint32_t structSize;
    uint32_t fftSize;
    uint32_t windowKind;
    uint32_t overlapNumerator;
    uint32_t overlapDenominator;
    uint32_t binCount;
    uint32_t algorithmId;
    uint32_t algorithmVersion;
    uint32_t unitId;
    uint32_t toleranceBits;
    uint32_t sampleRate;
    uint32_t channelMode;
} VnMonitorSpectrumPayload;

typedef struct VnMonitorMeterPayload {
    uint32_t structSize;
    uint32_t algorithmId;
    uint32_t algorithmVersion;
    uint32_t lufsSupported;
    uint32_t samplePeakBits;
    uint32_t truePeakBits;
    uint32_t rmsBits;
    uint32_t holdBits;
    uint32_t samplePeakUnitId;
    uint32_t truePeakUnitId;
    uint32_t rmsUnitId;
    uint32_t holdUnitId;
    uint32_t lufsMomentaryBits;
    uint32_t lufsShortTermBits;
    uint32_t lufsIntegratedBits;
    uint32_t reserved0;
} VnMonitorMeterPayload;

typedef struct VnMonitorGainReductionPayload {
    uint32_t structSize;
    uint32_t algorithmId;
    uint32_t algorithmVersion;
    uint32_t reductionDbBits;
    VnObjectId ownerStableId;
    uint64_t ownerInstanceHandle;
    uint64_t ownerGeneration;
} VnMonitorGainReductionPayload;

typedef struct VnMonitorAggregatePayload {
    uint32_t structSize;
    uint32_t reason;
    uint64_t firstSourceSequence;
    uint64_t lastSourceSequence;
    uint64_t recordCount;
    uint64_t byteCount;
} VnMonitorAggregatePayload;

typedef struct VnMonitorRemoteHandshake {
    uint32_t structSize;
    uint32_t securityProfile;
    uint32_t protocolMajor;
    uint32_t protocolMinor;
    uint64_t schemaFingerprint;
    uint64_t buildFingerprint;
    uint64_t programSemanticFingerprint;
    uint64_t capabilityFingerprint;
    uint32_t requestedPermissionMask;
    uint32_t reserved0;
    uint64_t clientNonce[4];
    uint64_t serverNonce[4];
} VnMonitorRemoteHandshake;

typedef struct VnMonitorRemoteFrameCounter {
    uint32_t structSize;
    uint32_t reserved0;
    uint64_t messageCounter;
} VnMonitorRemoteFrameCounter;

/* Optional host diagnostics. No registration transfers emitter ownership or
 * grants observation permissions. Call on the engine logic thread. */
typedef struct VnMonitorHostSessionV1 {
    uint32_t structSize;
    uint32_t emitterCapacity;
    uint64_t engineInstance;
    uint64_t sdkSession;
    uint64_t reserved0;
} VnMonitorHostSessionV1;

enum {
    VN_MONITOR_HOST_UNKNOWN = 0,
    VN_MONITOR_HOST_UNITY_EMITTER = 1,
    VN_MONITOR_HOST_UNITY_SPATIAL_SOURCE = 2,
    VN_MONITOR_HOST_STUDIO_DEFAULT_EMITTER = 3
};

#define VN_MONITOR_STUDIO_DEFAULT_GAME_OBJECT_ID 1ull

typedef struct VnMonitorHostRegistrationV1 {
    uint32_t structSize;
    uint32_t ownershipKind;
    uint64_t engineInstance;
    uint64_t sdkSession;
    VnEmitterHandle emitter;
    uint64_t localSequence;
    uint64_t registrationRevision;
    uint64_t reserved0;
} VnMonitorHostRegistrationV1;

/* Atomic host identity and bounded UTF-8 description. Lengths exclude NUL.
 * descriptionFlags bit 0 means a source description was truncated.
 * The exact V1 prefix remains unchanged; this is a separate optional API. */
typedef struct VnMonitorHostRegistrationV2 {
    uint32_t structSize;
    uint32_t ownershipKind;
    uint64_t engineInstance;
    uint64_t sdkSession;
    VnEmitterHandle emitter;
    uint64_t localSequence;
    uint64_t registrationRevision;
    uint64_t reserved0;
    uint16_t nameBytes;
    uint16_t pathBytes;
    uint16_t sceneBytes;
    uint16_t descriptionFlags;
    uint8_t name[64];
    uint8_t path[256];
    uint8_t scene[128];
} VnMonitorHostRegistrationV2;

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

typedef uint32_t VnBusStateFlags;
enum {
    VN_BUS_STATE_VALID = 1,
    VN_BUS_STATE_WORKING = 2,
    VN_BUS_STATE_RENDERED = 4
};

typedef struct VnBusState {
    VnBusId busId;
    uint64_t blockSequence;
    uint32_t busIndex;
    uint32_t busGeneration;
    VnBusStateFlags flags;
    float peakPreFader;
    float peakPostFader;
    float peakPostEffects;
} VnBusState;

/* Logic-thread, passive enumeration of live Bus definitions into caller storage.
 * Start at firstBusIndex = 0 and continue at outNextBusIndex until UINT32_MAX.
 * Entries carry their own completed audio blockSequence (one-based); they need
 * not come from the same block. VALID absent means no matching audio publication
 * is available yet, never proof of silence. VALID without WORKING is dormant
 * with zero peaks. WORKING without RENDERED describes a degraded silent block.
 * Queries neither wake Buses nor advance Update. Caller serializes with other
 * logic APIs; enumeration may change between calls when definitions change.
 * states may be NULL only when capacity is zero. outCount and outNextBusIndex
 * are required. This API has no fixed Bus count or array-length limit. */
VnResult Vn_CaptureBusStates(uint32_t firstBusIndex, VnBusState* states,
                           uint32_t capacity, uint32_t* outCount,
                           uint32_t* outNextBusIndex);

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


VnResult Vn_RemoteStart(const VnRemoteConfig* config);

VnResult Vn_RemoteStop(void);

VnResult Vn_RemoteGetStatus(VnRemoteStatus* outStatus);

#define VN_REMOTE_PROTOCOL_MAJOR 3u

typedef uint64_t VnRemoteClient;

enum {
    VN_REMOTE_CAPABILITY_SNAPSHOT = 1u,
    VN_REMOTE_CAPABILITY_CATALOG = 2u,
    VN_REMOTE_CAPABILITY_CONTROL = 4u,
    VN_REMOTE_CAPABILITY_MONITOR_RECORDS = 8u,
    VN_REMOTE_CAPABILITY_EVENT_CAPTURE = 16u,
    VN_REMOTE_CAPABILITY_EXECUTION_OBSERVATION = 32u
};

enum {
    VN_REMOTE_CLIENT_LOCAL_ONLY = 1u
};

enum {
    VN_REMOTE_CLIENT_REQUEST_SNAPSHOT = 1u,
    VN_REMOTE_CLIENT_REQUEST_CATALOG = 2u,
    VN_REMOTE_CLIENT_REQUEST_COMMAND = 3u,
    VN_REMOTE_CLIENT_REQUEST_MONITOR_START = 4u,
    VN_REMOTE_CLIENT_REQUEST_MONITOR_STOP = 5u,
    VN_REMOTE_CLIENT_REQUEST_EVENT_START = 6u,
    VN_REMOTE_CLIENT_REQUEST_EVENT_START_V2 = 7u,
    VN_REMOTE_CLIENT_REQUEST_EVENT_SEAL = 8u
};

enum {
    VN_REMOTE_OPERATION_POST_ACTION = 1u,
    VN_REMOTE_OPERATION_STOP_ALL = 3u,
    VN_REMOTE_OPERATION_STOP_VOICE = 4u,
    VN_REMOTE_OPERATION_STOP_ACTION = 5u,
    VN_REMOTE_OPERATION_SET_GLOBAL_VARIABLE = 6u,
    VN_REMOTE_OPERATION_SET_BUS_VOLUME = 7u,
    VN_REMOTE_OPERATION_SET_VOICE_VOLUME = 8u
};

enum {
    VN_REMOTE_CATALOG_ACTION = 1u,
    VN_REMOTE_CATALOG_VARIABLE = 2u,
    VN_REMOTE_CATALOG_BUS = 3u
};

enum {
    VN_REMOTE_CLIENT_EVENT_TARGET_FOUND = 1u,
    VN_REMOTE_CLIENT_EVENT_HANDSHAKING = 2u,
    VN_REMOTE_CLIENT_EVENT_CONNECTED = 3u,
    VN_REMOTE_CLIENT_EVENT_FAILED = 4u,
    VN_REMOTE_CLIENT_EVENT_REJECTED = 5u,
    VN_REMOTE_CLIENT_EVENT_SNAPSHOT = 6u,
    VN_REMOTE_CLIENT_EVENT_CATALOG = 7u,
    VN_REMOTE_CLIENT_EVENT_COMMAND_RESULT = 8u,
    VN_REMOTE_CLIENT_EVENT_MONITOR_STATUS = 9u,
    VN_REMOTE_CLIENT_EVENT_MONITOR_RECORDS = 10u,
    VN_REMOTE_CLIENT_EVENT_CAPTURE_STATUS = 11u,
    VN_REMOTE_CLIENT_EVENT_CAPTURE_RECORDS = 12u
};

enum {
    VN_REMOTE_CLIENT_FAILURE_CONNECT = 1u,
    VN_REMOTE_CLIENT_FAILURE_DISCONNECTED = 2u,
    VN_REMOTE_CLIENT_FAILURE_CONNECT_TIMEOUT = 3u,
    VN_REMOTE_CLIENT_FAILURE_RESPONSE_TIMEOUT = 4u,
    VN_REMOTE_CLIENT_FAILURE_COMMAND_TIMEOUT = 5u,
    VN_REMOTE_CLIENT_FAILURE_SILENT = 6u,
    VN_REMOTE_CLIENT_FAILURE_IN_USE = 7u,
    VN_REMOTE_CLIENT_FAILURE_INCOMPATIBLE = 8u,
    VN_REMOTE_CLIENT_FAILURE_PROTOCOL = 9u,
    VN_REMOTE_CLIENT_FAILURE_SEND = 10u,
    VN_REMOTE_CLIENT_FAILURE_SEQUENCE_EXHAUSTED = 11u
};

typedef struct VnRemoteClientDesc {
    uint32_t structSize;
    uint32_t flags;
    const char* clientName;
} VnRemoteClientDesc;

typedef struct VnRemoteClientRequest {
    uint32_t structSize;
    uint32_t kind;
    uint32_t operation;
    uint32_t durationMs;
    uint64_t expectedGeneration;
    VnObjectId target;
    uint64_t instanceHandle;
    uint64_t secondary;
    float value;
    uint32_t offset;
    uint32_t streamMask;
    uint32_t cadenceHz;
    uint32_t bytesPerSecond;
    uint32_t executionFactVersion;
    uint32_t permissionMask;
    uint32_t requestSymbols;
    uint64_t consumerQueueBytes;
    uint64_t sessionEpoch;
} VnRemoteClientRequest;

typedef struct VnRemoteClientTarget {
    VnResult result;
    uint32_t protocolMajor;
    uint32_t protocolMinor;
    uint32_t processId;
    uint32_t tcpPort;
    uint32_t sampleRate;
    uint32_t blockFrames;
    uint32_t compatible;
    uint64_t instanceId;
    uint64_t sessionId;
    uint64_t capabilities;
    char address[64];
    char bindAddress[64];
    char applicationName[128];
    char projectName[128];
    char machineName[128];
    char engineVersion[32];
    char buildId[128];
    char platform[32];
} VnRemoteClientTarget;

typedef struct VnRemoteClientSnapshot {
    VnResult result;
    uint32_t activeVoices;
    uint64_t blockIndex;
    uint32_t busCount;
    uint32_t callbackCpuUs;
    uint32_t updateCpuUs;
    uint32_t voiceRows;
    uint64_t commandBackpressure;
    uint64_t droppedNotifications;
    uint32_t busRows;
    uint32_t reserved0;
} VnRemoteClientSnapshot;

typedef struct VnRemoteClientCaptureStatus {
    VnResult result;
    uint32_t state;
    uint64_t sessionEpoch;
    uint64_t startedTimestampNs;
    uint64_t stoppedTimestampNs;
    uint64_t sourceDroppedRecords;
    uint64_t deliveryDroppedRecords;
    uint64_t transportDroppedRecords;
    uint32_t execution;
    uint32_t acceptedExecutionFactVersion;
    uint32_t baselineAvailability;
    uint32_t requestedPermissionMask;
    uint32_t acceptedPermissionMask;
    uint32_t deniedPermissionMask;
    uint32_t unsupportedPermissionMask;
    uint32_t acceptedStreamKindMask;
    uint32_t symbolsDecision;
    uint32_t symbolAvailability;
    uint64_t identityUnavailableCount;
    uint64_t baselineStateCutId;
    uint64_t supportedFactKinds[2];
} VnRemoteClientCaptureStatus;

typedef struct VnRemoteClientEvent {
    uint32_t structSize;
    uint32_t kind;
    uint32_t requestId;
    VnResult result;
    uint32_t failure;
    int32_t osError;
    uint32_t operation;
    uint32_t recordCount;
    uint64_t handle;
    uint64_t sessionEpoch;
    uint64_t planGeneration;
    uint32_t streamMask;
    uint32_t catalogTotal;
    uint64_t catalogGeneration;
    uint32_t catalogOffset;
    uint32_t catalogRows;
    uint64_t dataBytes;
    VnRemoteClientTarget target;
    VnRemoteClientSnapshot snapshot;
    VnRemoteClientCaptureStatus capture;
} VnRemoteClientEvent;

typedef struct VnRemoteClientCatalogRow {
    VnObjectId id;
    uint32_t kind;
    uint32_t reserved0;
} VnRemoteClientCatalogRow;

typedef struct VnRemoteClientVoice {
    uint64_t handle;
    VnObjectId soundId;
    uint32_t state;
    float peak;
} VnRemoteClientVoice;

typedef struct VnRemoteClientBus {
    VnObjectId id;
    uint32_t generation;
    float peak;
} VnRemoteClientBus;

VnResult Vn_RemoteClientCreate(const VnRemoteClientDesc* desc, VnRemoteClient* outClient);

VnResult Vn_RemoteClientDestroy(VnRemoteClient client);

VnResult Vn_RemoteClientDiscover(VnRemoteClient client, uint32_t discoveryPort);

VnResult Vn_RemoteClientConnect(VnRemoteClient client, const char* host, uint32_t tcpPort);

VnResult Vn_RemoteClientDisconnect(VnRemoteClient client);

VnResult Vn_RemoteClientRequest(VnRemoteClient client, const VnRemoteClientRequest* request,
                                uint32_t* outRequestId);

VnResult Vn_RemoteClientPoll(VnRemoteClient client, VnRemoteClientEvent* outEvent,
                             void* buffer, uint64_t bufferBytes);

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

/* Sends are stored in a caller-owned flat array. sendOffset/sendCount select
 * each path's range; there is no per-path Send capacity. */
typedef struct VnPreviewPlaybackPathV2 {
    uint32_t structSize;
    uint32_t rowCount;
    uint32_t sendCount;
    uint32_t reserved0;
    uint64_t sendOffset;
    uint64_t voiceInstance;
    VnObjectId soundId;
    VnObjectId outputBusId;
    VnPreviewPlaybackPathRowV1 rows[VN_PREVIEW_PATH_ROW_CAPACITY];
} VnPreviewPlaybackPathV2;

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

/* Logic-thread capture. Returns required counts even when a buffer is too small.
 * Only complete Send ranges are written. Retry after resizing both buffers;
 * required counts and records always describe the same capture. */
VnPreviewResult VnPreview_CapturePlaybackPathsV2(
    VnPreviewPlaybackPathV2* outPaths, uint32_t capacity, uint32_t* requiredCount,
    VnPreviewPlaybackSendV1* outSends, uint64_t sendCapacity, uint64_t* requiredSendCount);

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

typedef struct VnPreviewBusEffectV1 {
    VnBusId busId;
    VnObjectId effectId;
    uint32_t slot;
    uint32_t effectType;
    uint32_t descriptorVintage;
    uint32_t flags;
    uint32_t parameterFirst;
    uint32_t parameterCount;
} VnPreviewBusEffectV1;

typedef struct VnPreviewBusEffectParameterV1 {
    uint32_t index;
    float value;
} VnPreviewBusEffectParameterV1;

typedef struct VnPreviewProjectDescV4 {
    VnPreviewProjectDescV3 base;
    const VnPreviewBusEffectV1* busEffects;
    uint32_t busEffectCount;
    uint32_t busEffectParameterCount;
    const VnPreviewBusEffectParameterV1* busEffectParameters;
    const uint32_t* busFaderAfterSlots;
    uint32_t busFaderCount;
    uint32_t forceWorldReplacement;
} VnPreviewProjectDescV4;

typedef int32_t VnPreviewPrepareStatus;
enum {
    VN_PREVIEW_PREPARE_OK = 0,
    VN_PREVIEW_PREPARE_INVALID = 1,
    VN_PREVIEW_PREPARE_BACKPRESSURE = 2,
    VN_PREVIEW_PREPARE_CAPACITY = 3,
    VN_PREVIEW_PREPARE_UNSUPPORTED = 4,
    VN_PREVIEW_PREPARE_DEVICE_ERROR = 5,
    VN_PREVIEW_PREPARE_INVALID_WORLD = 6,
    VN_PREVIEW_PREPARE_UNAVAILABLE = 7
};

typedef int32_t VnPreviewCommitStatus;
enum {
    VN_PREVIEW_COMMIT_OK = 0,
    VN_PREVIEW_COMMIT_BACKPRESSURE = 1,
    VN_PREVIEW_COMMIT_INVALID = 2,
    VN_PREVIEW_COMMIT_DEVICE_ERROR = 3,
    VN_PREVIEW_COMMIT_UNAVAILABLE = 4
};

VnPreviewPrepareStatus VnPreview_PrepareProject(
    const VnPreviewProjectDescV1* desc, VnPreviewProjectHandle* outHandle);
HARKEN_DEPRECATED("Use VnPreview_PrepareProjectV4.")
VnPreviewPrepareStatus VnPreview_PrepareProjectV2(
    const VnPreviewProjectDescV2* desc, VnPreviewProjectHandle* outHandle);
HARKEN_DEPRECATED("Use VnPreview_PrepareProjectV4.")
VnPreviewPrepareStatus VnPreview_PrepareProjectV3(
    const VnPreviewProjectDescV3* desc, VnPreviewProjectHandle* outHandle);
VnPreviewPrepareStatus VnPreview_PrepareProjectV4(
    const VnPreviewProjectDescV4* desc, VnPreviewProjectHandle* outHandle);
VnPreviewCommitStatus VnPreview_CommitProject(
    VnPreviewProjectHandle handle, uint64_t* outGeneration);
void VnPreview_CancelProject(VnPreviewProjectHandle handle);
uint32_t VnPreview_DrainProjectAcks(void);
uint32_t VnPreview_ProjectIsOpen(uint64_t generation);
uint32_t VnPreview_QueryOwnerRevision(uint64_t ownerId, uint64_t* outRevision);
uint32_t VnPreview_HasPlayableConsumers(void);
uint32_t VnPreview_HasEffectChainConsumers(void);
uint64_t VnPreview_QueryVoiceWorldGeneration(uint64_t voiceInstance);
uint32_t VnPreview_HasWorldGeneration(uint64_t generation);
uint32_t VnPreview_HasAction(VnObjectId actionId);
void VnPreview_PumpNullMixer(uint32_t blockCount);
VnResult VnPreview_EnableChannelPeakMetering(void);
VnResult VnPreview_TakeChannelPeaks(VnPreviewChannelPeaksV1* outPeaks);
VnResult VnPreview_RegisterPcm(VnSoundId soundId, const VnPCMDesc* desc,
    uint64_t loopStartFrame, uint64_t loopEndFrame, uint64_t* outContentRevision);
VnResult VnPreview_RegisterResidentVorbis(VnSoundId soundId, const VnPackByteSpan* media,
    uint64_t* outContentRevision);
VnResult VnPreview_ValidateResidentVorbis(const VnPackByteSpan* media);
VnResult VnPreview_PlayVoiceRaw(const VnPlayDesc* desc, VnVoiceHandle* outHandle);
#define VN_PREVIEW_EFFECT_CHAIN_CAPACITY 8u

typedef struct VnPreviewVoiceEffectEntryV1 {
    VnObjectId effectId;
    uint32_t effectType;
    uint32_t bypass;
    float ceilingDb;
    float releaseMs;
} VnPreviewVoiceEffectEntryV1;

typedef struct VnPreviewVoiceEffectChainV1 {
    uint32_t structSize;
    uint32_t entryCount;
    VnPreviewVoiceEffectEntryV1 entries[VN_PREVIEW_EFFECT_CHAIN_CAPACITY];
} VnPreviewVoiceEffectChainV1;

/* Logic thread. One complete ordered Limiter chain (or entryCount=0) for a
 * current Preview playback-path voiceInstance, including its generation.
 * Entries require nonzero, unique effectId values; duplicate preset installs
 * have distinct entry ids. VN_OK means accepted configuration, not audio
 * completion. An identical accepted chain is a no-op. VN_BACKPRESSURE keeps
 * the previous accepted chain and asks the caller to retry its latest desired
 * value after normal Vn_Update/render progress. Stale handles are rejected.
 * Structural edits crossfade changed slots over 5 ms of produced samples;
 * unchanged entries preserve their DSP history. No per-call revision is
 * required: the single Runtime owner captures internal base/target revisions.
 */
VnResult VnPreview_SetVoiceEffectChain(uint64_t voiceInstance,
    const VnPreviewVoiceEffectChainV1* chain);

VnResult VnPreview_RefreshVoiceBindings(uint64_t voiceInstance);
VnResult VnPreview_SetVoiceProperty(uint64_t voiceInstance,
    uint32_t property, float value, VnBusId sendBusId);
VnResult VnPreview_SetVoiceEffect(uint64_t voiceInstance, VnObjectId effectId,
    uint32_t effectType, const float* parameters, uint32_t parameterCount,
    uint32_t bypass);
VnResult VnPreview_SetBusEffect(uint64_t ownerId, VnBusId busId, VnObjectId effectId,
    uint32_t effectType, const float* parameters, uint32_t parameterCount,
    uint32_t bypass);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HARKEN_HARKEN_DEVELOPMENT_H */
