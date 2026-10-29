/* Harken Core public C API.
 *
 * Public descriptors carry a structSize field as their ABI vintage. The
 * engine accepts only the exact struct size documented for each descriptor;
 * breaking descriptor replacements therefore reject older header vintages.
 * The Bezier curve contract is a coordinated breaking generation: pack schema
 * vintage 2, remote protocol major 2, and session trace format v2 reject their
 * older shape-tag generations rather than translating them.
 *
 * Threading contract: the API is externally synchronized — calls are
 * permitted from one thread at a time.
 *
 * Render floating-point contract: engine rendering executes with
 * flush-to-zero and denormals-are-zero behavior (or the platform equivalent).
 * Subnormal inputs and intermediate state are therefore treated as zero.
 * When rendering runs on a caller- or device-owned thread, the engine restores
 * that thread's incoming floating-point control state before returning.
 */
#ifndef HARKEN_HARKEN_H
#define HARKEN_HARKEN_H

#include <stdint.h>
#include <harken/ObjectId.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result code. 0 is success; negative ranges group error kinds:
 * -1xx invalid arguments, -2xx identity, -3xx capacity/state,
 * -4xx system/device. Invalid input is never undefined behavior. */
typedef int32_t VnResult;

enum {
    VN_OK = 0,

    VN_INVALID_ARG = -100,
    VN_INVALID_CONFIG = -101,

    VN_UNKNOWN_ID = -200,
    VN_STALE_HANDLE = -201,
    VN_CONTENT_NOT_LOADED = -220, /* identity known, owner Pack inactive */
    VN_AUTHORITY_CONFLICT = -221, /* Pack vs DirectWorld mutation in one session */
    VN_PACK_STALE_REVISION = -222, /* content revision / handle pairing mismatch */

    VN_NO_VOICE = -300,
    VN_BACKPRESSURE = -301,
    VN_IN_USE = -302,
    VN_NOT_INITIALIZED = -303,
    VN_ALREADY_INITIALIZED = -304,

    VN_DEVICE_ERROR = -400,

    VN_NO_BUS = -305,
    VN_NO_VARIABLE = -306,
    VN_NO_ATTENUATION = -307,

    VN_NO_PACK = -308,
    VN_NO_EFFECT = -309, /* an effect type's instance pool is exhausted */
    VN_BAD_PACK = -401,

    VN_NO_STREAM = -310,      /* the stream slot table is exhausted */
    VN_BAD_STREAM_FILE = -402, /* a .stream file is unreadable, malformed,
                                * or fails its checksums */

    /* Capacity/state codes for the playback-management surfaces, each
     * produced by the API that owns it: VN_NO_ACTION by
     * Action registration, VN_NO_CONTAINER by container registration,
     * VN_NO_EMITTER by emitter creation, VN_NO_SEND and VN_SEND_CYCLE by
     * the send-connection calls, and VN_EMITTER_ATTACHED by per-voice
     * spatial writes on a voice that follows an emitter. */
    VN_NO_ACTION = -311,      /* Action firing admission capacity is exhausted */
    VN_NO_CONTAINER = -312,   /* the container registry or child pool is full */
    VN_NO_EMITTER = -313,     /* the emitter handle table is exhausted */
    VN_NO_SEND = -314,        /* a send-slot table (voice or bus) is full */
    VN_SEND_CYCLE = -315,     /* a bus send edge would close a routing cycle */
    VN_EMITTER_ATTACHED = -316, /* a per-voice spatial write was refused because
                                * the voice follows an emitter (one owner of
                                * position/orientation, no silent race) */
    VN_TRIGGER_CYCLE = -317, /* an Action registration (or replacement) whose
                             * trigger Steps would close a cycle over the
                             * directed graph of trigger references among
                             * currently registered Actions, including a
                             * self-reference; refused with zero side effects */

    VN_TRACE_TOO_LATE = -318, /* Vn_TraceStart after the first stateful API
                               * call since Vn_Init: the session is no longer
                               * reconstructible from a journal alone (see the
                               * note at Vn_TraceStart) */

    VN_UNSUPPORTED_LAYOUT = -319, /* a mix/device/downmix channel layout the
                                   * build cannot honor (an out-of-range
                                   * VnChannelLayout, or a src/dst layout pair
                                   * this build does not fold) */

    VN_BAD_TRACE_FILE = -403, /* the trace file could not be created or opened
                               * for writing at the given path */

    VN_NO_BINDING = -320,     /* a binding pool (definitions, attachments, or a
                               * voice's instance slots) is exhausted; the log
                               * record names which */

    VN_METER_CYCLE = -321,    /* reserved control-graph cycle refusal value */

    VN_NO_DYNAMIC_ROUTE = -322,
    VN_DYNAMIC_ROUTE_CYCLE = -323,
    VN_DYNAMIC_ROUTE_ORDER = -324,
    VN_REVISION_CONFLICT = -325,
    VN_NO_TOPOLOGY_OWNER = -326,
    VN_CAPABILITY_UNAVAILABLE = -327,
    VN_RUNTIME_RECREATE_REQUIRED = -328,
    VN_CURRENT_WORLD_CAPACITY = -329,
    VN_CURRENT_WORLD_IO_ERROR = -404,
    VN_BAD_CURRENT_WORLD = -405,
    VN_PACK_INCOMPATIBLE = -410,
    VN_PACK_PATH_ESCAPE = -411,
    VN_PACK_HASH_MISMATCH = -412,
    VN_PACK_CYCLE = -413,
    VN_PACK_CATALOG_CAPACITY = -414,
    VN_PACK_RESIDENT_CAPACITY = -415,
    VN_PACK_PROVIDER_FAILED = -416,
    VN_PACK_INIT_LIFECYCLE = -417,

    /* Runtime Monitor result codes. Values are ABI-fixed. Unimplemented Monitor
     * entry points return VN_MONITOR_UNSUPPORTED when an engine is present and
     * VN_NOT_INITIALIZED when it is not. Do not fold these into VN_INVALID_ARG
     * or VN_BACKPRESSURE. */
    VN_MONITOR_WRONG_THREAD = -330,
    VN_MONITOR_STALE_SESSION = -331,
    VN_MONITOR_CONCURRENT_POLL = -332,
    VN_MONITOR_BUFFER_TOO_SMALL = -333,
    VN_MONITOR_UNSUPPORTED = -334,
    VN_MONITOR_PERMISSION_DENIED = -335,
    VN_MONITOR_STALE_LEASE = -336,
    VN_MONITOR_STALE_PLAN = -337,
    VN_MONITOR_STALE_TAP = -338,
    VN_MONITOR_PLAN_CAPACITY = -339,
    VN_MONITOR_SESSION_CAPACITY = -340,
    VN_MONITOR_PRODUCER_CAPACITY = -341,
    VN_MONITOR_STREAM_CAPACITY = -342,
    VN_MONITOR_BUDGET_EXCEEDED = -343,
    VN_MONITOR_INCOMPATIBLE = -344,
    VN_MONITOR_SEQUENCE_EXHAUSTED = -345,
    VN_MONITOR_RECORDING_FAILED = -346,
    VN_MONITOR_RESYNC_REQUIRED = -347,

    VN_VARIABLE_SCOPE_MISMATCH = -1200, /* reserved legacy result code */
    VN_VARIABLE_WRITER_FORBIDDEN = -1201, /* Game vs Action-only, or Action vs Game-only */
    VN_VARIABLE_EMITTER_SLOT_EXHAUSTED = -1202, /* per-emitter override slots full */
    VN_VARIABLE_TRANSACTION_EXHAUSTED = -1203, /* pending table full */
    VN_VARIABLE_POLICY_INCOMPLETE = -1204, /* overlay missing/extra vs Init rows */
    VN_VARIABLE_CATALOG_MISMATCH = -1205, /* typed SDK execution catalog mismatch */
    VN_NO_VARIABLE_LABEL = -1206,

    /* Gameplay catalog checked-name results. Values are ABI-fixed. */
    VN_GAMEPLAY_NAME_RETIRED = -1000,
    VN_GAMEPLAY_WRONG_PROJECT = -1001,
    VN_GAMEPLAY_CONTRACT_MISMATCH = -1002
};

/* Values for VnConfig.deviceMode. */
enum {
    VN_DEVICE_REAL = 0, /* real audio device output */
    VN_DEVICE_NULL = 1  /* headless: blocks are pumped by the host/test clock */
};

/* Notification record; the full definition appears after VnVoiceHandle
 * below. The callback runs only inside Vn_Update, on the pumping thread —
 * the engine never calls user code from the audio thread. */
typedef struct VnNotification VnNotification;
typedef void (*VnNotificationCallback)(const VnNotification* notification,
                                       void* userData);
/* Callback exception contract: callbacks must not throw across the C ABI.
 * A normal callback-side Vn_Term follows the existing deferred-teardown
 * re-entry contract. If a callback throws without first calling Vn_Term,
 * Vn_Update stops all five dispatch drains, returns VN_DEVICE_ERROR, and marks
 * the session fatally sticky; all entries except Vn_Term subsequently return
 * VN_NOT_INITIALIZED. Vn_Term remains the explicit idempotent cleanup path.
 * If a callback calls Vn_Term and then
 * throws, the existing deferred teardown still runs exactly once and the
 * update reports VN_DEVICE_ERROR without touching the detached engine. */

/* Log sink. Audio-side logging writes fixed-size binary records into a
 * lock-free ring; text exists only on the pumping thread — this callback
 * is invoked once per drained record inside Vn_Update, never on the audio
 * thread. `level` is a VN_LOG_* value, `code` identifies the condition,
 * and `text` is a formatted, NUL-terminated message valid only for the
 * duration of the call. */
typedef void (*VnLogFn)(void* userData, uint32_t level, uint32_t code,
                        const char* text);

/* Engine-wide capacities and modes, fixed at Vn_Init. Changing capacity is a
 * config change, never a code change. Zero in a numeric field selects the
 * engine default. Fields are appended only, never reordered or removed;
 * structSize tells the engine which vintage the caller built against, and a
 * shorter (older) struct reads as defaults for every appended field. */
typedef struct VnConfig {
    uint32_t structSize;     /* set to sizeof(VnConfig); versions this struct */
    uint32_t sampleRate;     /* Hz; 0 = native rate on a real device, with
                              * 48000 used by NullBackend or as query fallback */
    uint32_t blockFrames;    /* frames per render block; 0 = 256 */
    uint32_t maxVoices;      /* legacy field name: Render Voice capacity;
                              * 0 = 256 */
    uint32_t fxSlotsPerVoice;/* insert-effect slots per voice; 0 = 1 */
    uint32_t arenaBytes;     /* per-block bump arena size; 0 = 2097152 */
    uint32_t deviceMode;     /* VN_DEVICE_REAL or VN_DEVICE_NULL */
    uint32_t reserved0;      /* keeps 8-byte alignment for future fields */
    VnNotificationCallback onNotification; /* notification callback, or NULL
                                             * for none */
    void* notificationUserData; /* passed through to onNotification verbatim */
    uint32_t maxBuses;          /* bus tree capacity incl. master; 0 = 32 */
    uint32_t commandQueueDepth; /* logic->audio queue; 0 = 256; rounded up
                                 * to a power of two */
    uint32_t notificationQueueDepth; /* audio->logic queue; 0 = 256;
                                      * rounded up to a power of two; minimum
                                      * capacity is 2 */
    uint32_t reserved1;      /* keeps 8-byte alignment for future fields */
    VnLogFn logCallback;     /* log-record sink, or NULL for none */
    void* logUserData;       /* passed through to logCallback verbatim */
    uint32_t delayEffectPoolCapacity; /* feedback-delay instances shared across
                                       * all voice chains; 0 = 8 */
    uint32_t toneEffectPoolCapacity;  /* three-band-tone instances shared across
                                       * all voice chains; 0 = 16 */
    uint32_t streamRingFrames;   /* per-stream ring capacity in frames;
                                  * 0 = 16384; rounded up to a power of two */
    uint32_t maxStreams;         /* stream slot capacity; 0 = 16. A streamed
                                  * Render Voice also consumes one such slot. */
    uint32_t prebufferMs;        /* ring fill required before a streamed
                                  * voice starts sounding; 0 = 100 */
    uint32_t starvationCooldownBlocks; /* minimum blocks between successive
                                        * starvation notifications per voice;
                                        * 0 = 32 */
    uint32_t maxPacks;      /* pack table capacity; 0 = 256 */
    uint32_t reserved2;          /* keeps 8-byte alignment for future fields */
    /* Playback-management capacities (one struct-size vintage,
     * zero-means-default). A shorter struct omitting these reads every field
 * as its default. The capacities the current engine consumes — the Action,
     * container, emitter, random-seed, pending-action, and voice-stealing
     * fields — take effect immediately; the send/topology (and audibility
     * parking) capacities size features that later engine versions wire. A
     * config that leaves any of them zero behaves exactly as one built against
     * an older header vintage. */
    uint32_t maxActions;            /* Action registry capacity; 0 = 256 */
    uint32_t maxContainers;         /* container-asset registry capacity; 0 = 64 */
    uint32_t maxContainerChildren;  /* global container-child pool; 0 = 1024 */
    uint32_t containerCursorCapacity; /* per (container, emitter) selection-state
                                       * entries; 0 = 256 */
    uint32_t pendingActionCapacity; /* current struct: required-zero legacy
                                     * tombstone; legacy prefix vintages:
                                     * delayed-Step queue depth; 0 = 256 */
    uint32_t maxEmitters;           /* emitter handle capacity; 0 = 128 */
    uint32_t emitterVariableSlots;  /* per-emitter local Variable slots; 0 = 8 */
    uint32_t maxParkedVoices;       /* parked Virtual Voice capacity; 0 = 256.
                                     * Pending starts are Logical Voices but
                                     * are not Virtual Voices. */
    uint32_t voiceStealPolicy;      /* 0 = refuse (default), 1 = steal-quietest,
                                     * 2 = steal-oldest */
    uint32_t audibilityParkEnabled; /* 0 = off (default; prior behavior is
                                     * preserved bit-for-bit), 1 = park inaudible
                                     * voices: a voice whose audibility metric
                                     * falls below the threshold releases its
                                     * render slot but keeps its handle and
                                     * timeline (virtual time advances at pitch),
                                     * and returns when it rises past the fixed
                                     * +6 dB hysteresis. The metric is the
                                     * combine-resolved per-voice volume in dB
                                     * (API-direct + Variable bindings +
                                     * attenuation slots); it excludes bus gain
                                     * and metering. Scope of this engine
                                     * version: resident PCM one-shots, finite
                                     * multi-pass loops, and infinite loops park;
                                     * streamed voices stay live when inaudible.
                                     * A deferred start that
                                     * arrives inaudible parks directly without
                                     * ever rendering. A parked voice's return is
                                     * driven by distance attenuation — a
                                     * cone-angle change alone does not trigger
                                     * return while parked. Notifications
                                     * VN_NOTIFY_VOICE_PARKED /
                                     * VN_NOTIFY_VOICE_RETURNED are advisory. */
    float parkThresholdDb;          /* park below this level; 0 selects the
                                     * documented default -60 dB (the enable
                                     * flag, not the threshold, gates parking) */
    uint32_t randomSeed;            /* engine PRNG seed; 0 = documented default
                                     * constant */
    uint32_t voiceSendSlots;        /* per-voice send slots; 0 = 2 */
    uint32_t busSendSlotsPerBus;    /* per-bus send-edge slots; 0 = 4 */
    uint32_t fxSlotsPerBus;         /* per-bus insert-effect slots; 0 = 8 */
    uint32_t topologyPlanPoolCapacity; /* published bus-topology plan pool;
                                        * 0 = 4 */
    uint32_t reserved3;             /* keeps 8-byte alignment for future fields */
    uint32_t reserved4;             /* reserved for a future appended field */
    /* Action-extension addition (one struct-size vintage; a shorter struct
     * omitting it reads the default). */
    uint32_t maxTriggerDepth;       /* maximum nesting level of Action firings
                                     * within one chain: Vn_PostAction executes
                                     * at depth 1 and a Post Action Step at
                                     * depth d fires its target at depth d + 1; a nested
                                     * firing beyond the ceiling is dropped at
                                     * fire time and counted. 0 = 8; above 64
                                     * refused VN_INVALID_ARG. */
    uint32_t reserved5;             /* keeps 8-byte alignment for future fields */
    /* Multichannel-output and job-parallel additions (one struct-size vintage,
     * appended after reserved5; every feature off/neutral by default so a
     * shorter struct omitting these behaves exactly as an M4-vintage config).
     * Both layout fields follow the same rule: VN_CHANNEL_LAYOUT_DEFAULT (0)
     * means "this field's documented default", never a concrete layout, so the
     * zero-tail read of an older struct is stable. */
    uint32_t mixChannelLayout;      /* VnChannelLayout: the engine's internal
                                     * bus-tree render width, fixed at Init.
                                     * VN_CHANNEL_LAYOUT_DEFAULT (0) = the
                                     * documented default, stereo — at which the
                                     * multichannel amendment is inert. */
    uint32_t deviceChannelLayout;   /* VnChannelLayout: preferred device layout.
                                     * VN_CHANNEL_LAYOUT_DEFAULT (0) = follow the
                                     * device's native/default layout (on the
                                     * null backend, the mix layout). When the
                                     * resolved device layout is narrower than
                                     * the mix, the master->device output downmix
                                     * runs. */
    uint32_t renderWorkerCount;     /* job-parallel voice-render workers: 0 =
                                     * serial in-callback rendering (default,
                                     * bit-identical to M4); N = N workers. The
                                     * Null backend admits offline workers. A
                                     * real backend admits them only when every
                                     * worker can join the callback's verified
                                     * platform real-time scheduling domain;
                                     * otherwise Vn_Init/Vn_Resume returns
                                     * VN_DEVICE_ERROR. Above 64 Vn_Init returns
                                     * VN_INVALID_CONFIG. */
    uint32_t reserved6;             /* keeps 8-byte alignment for future fields */
    /* Public binding-system pool capacities (one struct-size vintage,
     * zero-means-default). A shorter struct omitting these reads every field
     * as its default, and a config that leaves them zero behaves exactly as
     * one built against an older header vintage. */
    uint32_t maxBindings;           /* binding definition pool; 0 = 128 */
    uint32_t maxBindingAttachments; /* global binding attachment pool; 0 = 256 */
    uint32_t voiceBindingInstanceSlots; /* per-voice binding instance slots (the
                                     * instance pool is maxVoices times this);
                                     * 0 = 8; above the per-voice slot-array
                                     * capacity (8) Vn_Init refuses
                                     * VN_INVALID_CONFIG */
    uint32_t meterRingBlocks;       /* meter transport ring depth in blocks
                                     * (the audio->logic peak record ring
                                     * behind bus meter Variables); 0 = 32;
                                     * values below 8 resolve to 8 — the
                                     * compressed channel needs headroom for
                                     * its validated take; rounded up to a
                                     * power of two; above 1024 Vn_Init
                                     * refuses VN_INVALID_CONFIG. A full
                                     * ring max-holds peaks into one
                                     * compressed overflow record (never
                                     * silently lost, at worst
                                     * time-compressed) and counts the
                                     * overflow. */
    /* Gain insert-effect pool capacity (one struct-size vintage,
     * zero-means-default). A shorter struct omitting it reads the default, so a
     * config that leaves it zero behaves exactly as one built against an older
     * header vintage. */
    uint32_t gainEffectPoolCapacity; /* Gain insert-effect instances shared
                                      * across all voice and bus chains; 0 = 16 */
    uint32_t reserved7;              /* keeps 8-byte alignment for future fields */
    /* Limiter insert-effect pool capacity (one struct-size vintage,
     * zero-means-default). A shorter struct omitting it reads the default. */
    uint32_t limiterEffectPoolCapacity; /* Limiter instances shared across all
                                         * voice and bus chains; 0 = 16 */
    uint32_t reserved8;                 /* keeps 8-byte alignment for future fields */
    /* Compressor and Dynamic Modulation / Sidechain capacities. A shorter
     * struct omitting this vintage reads every field as its default. */
    uint32_t compressorEffectPoolCapacity; /* Compressor instances shared across
                                            * voice and bus chains; 0 = 16 */
    uint32_t dynamicRouteCapacity;         /* effect-private dynamic routes;
                                            * 0 = 64 */
    uint32_t dynamicRoutePlanPoolCapacity; /* immutable route plans; 0 = 4 */
    uint32_t dynamicBusTapCapacity;        /* read-only bus audio taps; 0 = 16 */
    uint32_t dynamicVoiceTapCapacity;      /* read-only voice audio taps; 0 = 16 */
    uint32_t reserved9;                    /* keeps 8-byte alignment */
    /* Atomic Bus topology snapshot capacities, and the per-block control-command
     * ceiling that bounds the batches they publish. A shorter struct omitting
     * this vintage reads the documented defaults. The command ceiling occupies
     * the alignment slot this vintage already carried, so naming it changes no
     * offset and no struct size. */
    uint32_t maxBusTopologyOwners;         /* retained owner revision records;
                                            * 0 = 64 */
    uint32_t maxControlCommandsPerQuantum; /* most control commands one render
                                            * block applies; 0 = 256. Commands
                                            * past the ceiling apply on a later
                                            * block rather than being dropped,
                                            * so a value below what a caller
                                            * pushes costs latency, not changes:
                                            * a ramp that lands later starts
                                            * from a further-advanced value, and
                                            * the final value is unaffected.
                                            * Deliberately not derived from
                                            * commandQueueDepth -- a deeper
                                            * queue is precisely the
                                            * configuration that needs the
                                            * ceiling. It also bounds an
                                            * indivisible batch: an operation
                                            * publishing more commands at once
                                            * than this can never be applied, so
                                            * Vn_LoadPack and the Bus-topology
                                            * entry points refuse it with
                                            * VN_INVALID_CONFIG rather than
                                            * VN_BACKPRESSURE. Above 1048576
                                            * Vn_Init refuses
                                            * VN_INVALID_CONFIG. */
    uint64_t maxTopologyPlanBytes;         /* aggregate topology-plan pool byte
                                            * budget; 0 = 256 MiB */
    uint32_t maxActionFirings;             /* retained Action firing rows;
                                            * 0 = 256 */
    uint32_t reserved10;                   /* keeps the appended vintage explicit */
    /* Pack catalog / resident budgets (one struct-size vintage). A shorter
     * struct omitting these reads every field as its default. */
    uint32_t maxCatalogEntries;            /* Init routing-catalog identities;
                                            * 0 = 65536 */
    uint32_t reserved11;                   /* keeps 8-byte alignment; must be 0 */
    uint64_t maxCatalogBytes;              /* catalog payload budget; 0 = 4 MiB */
    uint64_t maxResidentDefinitionBytes;   /* loaded partition-body budget;
                                            * 0 = 32 MiB */
    uint64_t maxCompositeIndexBytes;       /* published identity-index budget;
                                            * 0 = 2 MiB */
    uint64_t maxPackDecodeStagingBytes;    /* encoded Pack + decode staging;
                                            * 0 = 64 MiB */
    uint64_t maxPackResourceBytes;         /* resident resource budget;
                                            * 0 = existing pack/media limits */
    uint64_t maxPackReplacementPeakBytes;  /* old + retiring + staging peak;
                                            * 0 = 96 MiB */
    uint32_t maxActionCooldownRows;         /* committed Action cooldown rows;
                                            * 0 = 256 */
    uint32_t reserved12;                    /* must be 0 */
} VnConfig;

/* Create the engine and preallocate every pool. All capacity is fixed here;
 * nothing allocates on the audio thread afterwards. */
VnResult Vn_Init(const VnConfig* config);

/* Release the platform audio device while preserving the engine, mixer,
 * voices, streams, and content state. Audio-frame time does not advance while
 * the device is suspended. The host must call this from its externally
 * synchronized lifecycle thread when entering the background or lock state.
 * If stopping the device fails, device destruction still completes and the
 * engine remains suspended with no live device. VN_DEVICE_ERROR reports the
 * stop failure but does not require termination; Vn_Resume remains valid and
 * retryable. Vn_Update remains valid and continues pumping notifications and
 * callbacks and publishing commands, but autonomous voice-lifecycle action is
 * deferred; audibility parking and return resume on the first Vn_Update after
 * Vn_Resume. On Windows, endpoint/default-device changes are host-owned: call
 * Vn_Suspend and then Vn_Resume from this synchronized lifecycle thread.
 * Repeated calls are successful no-ops. */
VnResult Vn_Suspend(void);

/* Recreate and start the platform audio device after Vn_Suspend. A device
 * failure leaves the complete engine suspended so the host can retry this
 * call. The host must call this from its externally synchronized lifecycle
 * thread. Repeated calls while already running are successful no-ops. */
VnResult Vn_Resume(void);

/* Stop the device (if any) and free everything. Valid while running,
 * suspended, or waiting to retry a failed resume. */
VnResult Vn_Term(void);

typedef VnObjectId VnSoundId;
typedef VnObjectId VnBusId;

/* The master bus pre-exists with this fixed ID; it is never created or
 * destroyed by the caller. */
#define VN_BUS_MASTER VN_OBJECT_ID(0, 1)

/* Values for VnPCMDesc.layout: how the sample data is arranged. */
enum {
    VN_PCM_LAYOUT_INTERLEAVED = 0, /* frame-major: L R L R ...            */
    VN_PCM_LAYOUT_PLANAR = 1       /* channel-major: one plane per channel */
};

/* Values for VnPCMDesc.policy: who owns the sample memory after
 * registration. Under both policies the data is immutable once registered. */
enum {
    VN_PCM_POLICY_COPY = 0,     /* engine copies; caller memory is free to go */
    VN_PCM_POLICY_REFERENCE = 1 /* engine references; caller keeps the memory
                                 * pinned and unchanged until unregister */
};

/* Describes caller PCM for registration. Exactly one of the two sample
 * pointers is read, selected by layout: samplesInterleaved for
 * VN_PCM_LAYOUT_INTERLEAVED, samplesPlanar (an array of `channels` plane
 * pointers) for VN_PCM_LAYOUT_PLANAR. */
typedef struct VnPCMDesc {
    uint32_t structSize;  /* set to sizeof(VnPCMDesc); versions this struct */
    uint32_t sampleRate;  /* Hz of the source material; may differ from the
                           * engine rate (the voice resampler converts) */
    uint32_t channels;    /* 1..8 */
    uint32_t layout;      /* VN_PCM_LAYOUT_* */
    uint32_t policy;      /* VN_PCM_POLICY_* */
    uint32_t reserved0;   /* keeps 8-byte alignment for the fields below */
    uint64_t frames;      /* sample frames per channel; must be > 0 */
    const float* samplesInterleaved;
    const float* const* samplesPlanar;
} VnPCMDesc;

/* Registers PCM under an id. After registration the data is immutable.
 * Registering an id that already exists is refused with VN_IN_USE. */
VnResult Vn_RegisterPCM(VnSoundId soundId, const VnPCMDesc* desc);

/* Removes a registered sound. Refused with VN_IN_USE while any voice is
 * playing it. */
VnResult Vn_UnregisterPCM(VnSoundId soundId);

/* Engine-minted generational voice handle: low 32 bits slot index, high 32
 * bits generation. Generation increments on slot reuse, so a stale handle is
 * detected structurally and every operation on it is a benign no-op
 * returning VN_STALE_HANDLE. Zero is never a valid handle. */
typedef uint64_t VnVoiceHandle;

/* Engine-minted generational handle for an emitter — a position/orientation
 * source many voices can follow. Same scheme as a voice handle (low 32 bits
 * slot, high 32 bits generation, 0 never valid). Created with Vn_CreateEmitter
 * and released with Vn_DestroyEmitter; a stale handle is detected structurally
 * and every operation on it is a benign no-op returning VN_STALE_HANDLE. */
typedef uint64_t VnEmitterHandle;

/* Describes one raw Voice start request. Zero in volumeDb means unity gain (0 dB is
 * unity by definition); zero in pitchRatio selects the default ratio 1.0.
 *
 * spatialized selects the voice's spatial mode for life (it cannot change
 * after play): 0 renders the voice 2D — positions are stored but have no
 * acoustic effect; 1 renders it as a 3D point
 * emitter — the engine derives panning (and, once an attenuation profile
 * is bound, distance-driven parameters) from the emitter and listener
 * positions on every update. Spatialization accepts mono and stereo sources;
 * wider sources return VN_INVALID_ARG. Stereo channels retain independent
 * processing before the spatial matrix folds them into the output image.
 * Centered mono preserves the original sample level in each stereo output
 * channel in both 2D and 3D. Non-mono layouts use a unity-center equal-power
 * pan law (total squared gain 2); native mono mix stays unity. Centered
 * 2D stereo preserves its original image.
 *
 * loopCount is a struct-size-versioned tail field: 1 = a single pass (the
 * default, and what an older caller whose struct predates the field reads
 * as), 0 = loop forever until stopped, N = exactly N passes. The loop
 * region is the sound's loop points when it carries loopable pack metadata,
 * otherwise the whole file; passes join seamlessly.
 *
 * emitterHandle and priority are a further struct-size vintage. priority is
 * 0..255 (higher wins) and drives the steal policy when the voice pool is
 * full; 0 is a valid (lowest) priority, and a caller whose struct predates
 * this field reads as the default 128. emitterHandle (0 = none) names the
 * emitter the voice follows: the voice takes the emitter's position and
 * orientation each update and resolves Variable reads at the emitter's scope,
 * and its per-voice Vn_SetVoicePosition / Vn_SetVoiceOrientation are refused
 * VN_EMITTER_ATTACHED while the emitter is live. A nonzero handle that does
 * not name a live emitter refuses the play with VN_STALE_HANDLE.
 *
 * reserved0 carries the play flags (VN_PLAY_FLAG_*). It is a bit field, not an
 * enumeration: each capability owns one bit, so a later one is added by
 * lighting a spare bit rather than by redefining the field. Zero selects the
 * ordinary immediate start. Undefined bits are refused, never ignored — the
 * field makes no forward-compatibility promise, so a value this engine does
 * not define is a caller error rather than a hint. Flags are accepted only
 * from the full-size vintage: they pair with entry points an older header does
 * not declare. */
typedef struct VnPlayDesc {
    uint32_t structSize; /* set to sizeof(VnPlayDesc); versions this struct */
    uint32_t reserved0;  /* play flags; see VN_PLAY_FLAG_* below */
    VnSoundId soundId;   /* registered PCM or stream Sound id */
    VnBusId busId;       /* target bus; VN_BUS_MASTER or any created bus */
    float volumeDb;      /* voice gain in dB; 0 = unity */
    float pitchRatio;    /* playback rate multiplier; 0 = 1.0 */
    float positionX;     /* initial emitter position (read when
                          * spatialized = 1; stored otherwise) */
    float positionY;
    float positionZ;
    uint32_t spatialized; /* 0 = 2D (default), 1 = 3D point emitter */
    uint32_t loopCount;   /* 1 = single pass (default), 0 = infinite,
                           * N = N passes; a struct predating this field
                           * reads as 1 */
    uint32_t reserved1;   /* keeps the struct free of implicit tail padding
                           * (soundId forces 8-byte alignment); reserved for
                           * a future appended field */
    VnEmitterHandle emitterHandle; /* 0 = none; else the emitter this voice
                                    * follows (see the note above) */
    uint32_t priority;    /* 0..255 steal priority, higher wins; a struct
                           * predating this field reads as 128 */
    uint32_t reserved2;   /* keeps 8-byte alignment for future fields */
} VnPlayDesc;

/* VnPlayDesc.reserved0 flag bits.
 *
 * bit 0 (VN_PLAY_FLAG_STAGED_INSTALL) makes the play a staged install: it
 *   resolves and reserves everything but publishes nothing, and
 *   Vn_CommitVoiceStart later emits the start and staged values as consecutive
 *   command records during one engine call. See there.
 * bits 1-4 are reserved for the staged install's planned extensions —
 *   demanding a physical Render slot, reporting the Render identity alongside
 *   the public handle, observing the audio side's application of the committed
 *   batch, and taking ownership of a timed-out commit's cleanup. They are not
 *   accepted yet: setting one refuses the play.
 * bits 5-31 are unassigned. */
#define VN_PLAY_FLAG_STAGED_INSTALL ((uint32_t)0x00000001u)

/* Commits a staged install by publishing the play command and every value the
 * caller staged onto the voice as one indivisible command batch. Queue capacity
 * and the per-quantum command ceiling are checked before publication; the audio
 * drain waits until one quantum has room for the complete batch, so no rendered
 * block can observe a prefix. The first audible block therefore carries all
 * authored gain, pitch, pan, width, spatial mix, sends and effects.
 *
 * The staged install is a two-phase start. Runtime-owned installation with
 * VN_PLAY_FLAG_STAGED_INSTALL resolves the Sound, pins its attachment set,
 * claims the Logical identity and reserves the sources it needs, then returns a
 * handle WITHOUT publishing anything: the voice is installed but silent, and no
 * Render slot is claimed yet. The caller installs the rest through the ordinary
 * per-voice setters, which store onto a not-yet-started voice exactly as they
 * do for any pending start. This call ends the install: it admits the Render
 * slot and queue capacity for the complete command run, then emits it.
 *
 * VN_OK means the start is committed. That need not mean the
 * voice is already live: an install whose resolved onset is still ahead, one
 * that had to steal a Render slot, and one admitted as a virtual voice all
 * report VN_OK and land later. When their start batch is eventually published,
 * the same indivisible application rule applies.
 *
 * Refusals leave the staged install untouched and still cancellable, so the
 * caller's only obligation is to roll back with Vn_Stop, which cancels a
 * staged voice and cannot fail: VN_INVALID_ARG when the handle does not name a
 * staged install (an already-started voice, or a pending start that is waiting
 * on its own onset rather than on a commit), VN_STALE_HANDLE when it names
 * nothing, VN_NO_VOICE when the start cannot be admitted — no Render slot is
 * available, or the command queue cannot take the whole batch at once.
 *
 * That last refusal is deliberately NOT VN_BACKPRESSURE, unlike an ordinary
 * immediate raw Voice start: backpressure promises that the identical call succeeds once the
 * queue drains, which only helps a caller that can wait and retry. An installer
 * is mid-install and cannot — it holds a voice whose properties are already
 * staged — so the shortfall is reported as a plain admission failure and the
 * install is rolled back rather than retried. Do not "unify" this back onto the
 * backpressure contract.
 *
 * Logic thread only. */
VnResult Vn_CommitVoiceStart(VnVoiceHandle handle);

/* Stops a voice. fadeMs == 0 cuts at the next block boundary; otherwise
 * gain ramps linearly to zero over fadeMs, then the voice ends. Stopping an
 * already-ended (stale) handle is a benign no-op returning VN_STALE_HANDLE;
 * a full command queue returns VN_BACKPRESSURE with no state change. */
VnResult Vn_Stop(VnVoiceHandle handle, uint32_t fadeMs);

/* One non-Master Bus row in a complete owner-scoped topology snapshot.
 * flags is reserved and must be zero. Rows are read synchronously and are not
 * retained. */
typedef struct VnBusTopologyBus {
    uint32_t structSize; /* set to sizeof(VnBusTopologyBus) */
    uint32_t flags;      /* reserved; must be zero */
    VnBusId busId;
    VnBusId parentId;
} VnBusTopologyBus;

/* One Bus Send row in a complete owner-scoped topology snapshot. A retained
 * edge whose level changes ramps for rampMs; a new edge snaps. */
typedef struct VnBusTopologySend {
    uint32_t structSize; /* set to sizeof(VnBusTopologySend) */
    uint32_t rampMs;
    VnBusId fromBusId;
    VnBusId toBusId;
    float levelDb;
    uint32_t reserved0; /* must be zero */
} VnBusTopologySend;

/* Complete Bus and Bus Send state owned by one nonzero ownerId. Master is
 * implicit and omitted. Rows may be in any order. revision/baseRevision are
 * optimistic-concurrency tokens: an exact retry is idempotent; a stale or
 * different same-revision payload returns VN_REVISION_CONFLICT. A changed
 * snapshot publishes exactly one topology command and becomes audible at the
 * next block boundary. Every refusal has zero accepted-state side effects. */
typedef struct VnBusTopologyDesc {
    uint32_t structSize; /* set to sizeof(VnBusTopologyDesc) */
    uint32_t flags;      /* reserved; must be zero */
    uint64_t ownerId;    /* nonzero; zero belongs to compatibility APIs */
    uint64_t baseRevision;
    uint64_t revision;
    uint32_t busCount;
    uint32_t busStructSize;
    const VnBusTopologyBus* buses;
    uint32_t sendCount;
    uint32_t sendStructSize;
    const VnBusTopologySend* sends;
} VnBusTopologyDesc;

VnResult Vn_ApplyBusTopology(const VnBusTopologyDesc* topology);

/* Creates a bus under an existing parent. The tree is rooted at the
 * pre-existing master bus (VN_BUS_MASTER). Refusals: busId 0 is
 * VN_INVALID_ARG, an id already in use (including the master id) is
 * VN_IN_USE, an unknown parent is VN_UNKNOWN_ID, and a full bus table is
 * VN_NO_BUS. A create is a structural change: it publishes a topology plan,
 * so it can also return VN_BACKPRESSURE when a resource is momentarily
 * unavailable — a full command queue (clears after the audio thread drains a
 * block) or an exhausted topology-plan pool (clears after Vn_Update drains
 * retired plans). The four validation refusals above are checked first and
 * independent of queue state, and a refused create — for any cause — has no
 * side effects: the bus is not created and no plan is published. A destroyed
 * bus's id (see Vn_DestroyBus) is free again and creates a FRESH bus. Core
 * may reuse a free dense slot with a new generation; retained references are
 * generation-checked so the new bus cannot inherit stale render state. */
VnResult Vn_CreateBus(VnBusId busId, VnBusId parentId);

/* Sets a bus's volume. volumeDb must be finite, and the reduced bus volume
 * after this API-direct contribution must convert to a finite non-negative
 * linear gain (VN_INVALID_ARG otherwise).
 * rampMs > 0 ramps linearly in the amplitude domain from the current value,
 * completing on a block boundary; rampMs == 0 snaps at the next block
 * boundary. The first volume ever set on a bus snaps regardless of rampMs.
 * The master bus accepts volume changes like any other bus. A full command
 * queue returns VN_BACKPRESSURE with no state change. */
VnResult Vn_SetBusVolume(VnBusId busId, float volumeDb, uint32_t rampMs);

/* Sets this bus's mix-wide lowpass (filterKind 0) or highpass (filterKind 1).
 * The filter sits after Bus Volume and before post-fader taps, so both the
 * Direct output and sends sourced by this bus hear it. enable01 == 0 bypasses
 * (the default); enable01 == 1 uses cutoffHz and q, with a lowpass at or above
 * Nyquist and a highpass at or below 0 Hz also bypassing. cutoffHz and q must
 * be finite, and an enabled q must be positive. An unknown bus is
 * VN_UNKNOWN_ID; a full command queue is VN_BACKPRESSURE with no state change.
 * Coefficients are prepared on the logic thread. This Sprint does not bind
 * Variable or persist the cutoff in a pack. */
VnResult Vn_SetBusFilter(VnBusId busId, uint32_t filterKind, float cutoffHz,
                         float q, uint32_t enable01, uint32_t rampMs);

/* Replaces the independent gain on this bus's main output to its parent;
 * on VN_BUS_MASTER it scales the final output. Sends sourced by this bus
 * are unaffected, and ordinary Volume and Variable contributions remain
 * unchanged. volumeDb must convert to a finite non-negative linear gain
 * (VN_INVALID_ARG otherwise). The change starts at the next audio block:
 * rampMs == 0 snaps, while a positive ramp interpolates linearly in
 * amplitude from the current gain over ceil(rampMs * sampleRate / 1000)
 * frames, including the first update.
 * Returns VN_OK when queued, VN_UNKNOWN_ID for an unknown bus, or
 * VN_BACKPRESSURE when the command queue cannot admit the change; refusals
 * leave the gain unchanged. An absent or failed engine returns
 * VN_NOT_INITIALIZED; an unexpected internal failure returns VN_DEVICE_ERROR. */
VnResult Vn_SetBusOutputVolume(VnBusId busId, float volumeDb, uint32_t rampMs);

/* Sets the Direct/output-only lowpass (0) or highpass (1) on this bus.
 * Sends sourced by this bus are unaffected; the filter sits after Output
 * Volume on the path that continues to the parent (or the final mix on
 * VN_BUS_MASTER). Same argument and refusal contract as Vn_SetBusFilter,
 * except an unknown bus is resolved like Vn_SetBusOutputVolume
 * (VN_UNKNOWN_ID, including when Preview occupies Master). */
VnResult Vn_SetBusOutputFilter(VnBusId busId, uint32_t filterKind,
                               float cutoffHz, float q, uint32_t enable01,
                               uint32_t rampMs);

/* Mutes or unmutes a bus. Mute is logic-side state composed at the bus's
 * volume publish point — it never overwrites the API-direct volume base or
 * Variable-driven contributions, and unmuting restores the composed value
 * exactly. While muted the bus's published gain is a silence floor (linear
 * zero); fadeMs rides the bus-volume ramp discipline (rampMs semantics,
 * block-boundary completion — including the first-ever-value snap rule).
 * Muting the master silences the mix like any other bus. Setting the state a
 * bus already holds is a benign no-op returning VN_OK that publishes
 * nothing. Refusals, all with zero side effects: mute01 above 1 is
 * VN_INVALID_ARG; an unknown bus is VN_UNKNOWN_ID; a full command queue is
 * VN_BACKPRESSURE. Logic thread only. */
VnResult Vn_SetBusMute(VnBusId busId, uint32_t mute01, uint32_t fadeMs);

/* Solos or unsolos a bus. Solo audibility is tree-path scoped: while any bus
 * is soloed, the soloed buses' subtrees plus their ancestor chains to the
 * master stay audible and every other bus receives an effective mute (the
 * same silence floor as Vn_SetBusMute, composed with — never overwriting —
 * the bus's volume state); clearing the last solo restores every bus.
 * Send-fed paths do not follow solo: a send whose source is on a solo path
 * still scatters, but a target outside every solo path stays silenced by its
 * own effective mute. Soloing the master is accepted and benign (its path is
 * the whole tree). A solo flip republishes the effective gain of every bus
 * whose audibility changed, as one atomic call: the needed commands are
 * admission-checked up front and a shortage refuses the whole call with
 * VN_BACKPRESSURE and zero side effects. fadeMs applies to every republished
 * gain. Setting the state a bus already holds is a benign no-op returning
 * VN_OK that publishes nothing. Refusals, all with zero side effects: solo01
 * above 1 is VN_INVALID_ARG; an unknown bus is VN_UNKNOWN_ID. Logic thread
 * only. */
VnResult Vn_SetBusSolo(VnBusId busId, uint32_t solo01, uint32_t fadeMs);

/* Moves a live bus (and implicitly its whole subtree) under a new parent.
 * A structural change: it publishes one topology plan and the audio thread
 * installs the new parent atomically at a block boundary — the tree fold
 * never reads a half-applied graph, and the bus's volume, effects, sends,
 * and routed voices all travel with it. Reparenting onto the current parent
 * is a benign no-op returning VN_OK that republishes nothing. When solo is
 * active the audible set follows the new tree paths in the same call (the
 * needed gain republishes are admission-checked together with the plan).
 * Refusals, all with zero side effects: an unknown bus or parent is
 * VN_UNKNOWN_ID; the master bus, or newParentId == busId, is
 * VN_INVALID_ARG; an edge that would close a routing cycle through the
 * combined tree+send graph (including reparenting under the bus's own
 * subtree) is VN_SEND_CYCLE; a full topology-plan pool or command queue is
 * VN_BACKPRESSURE (retry after Vn_Update drains plan retires, or after an
 * audio block drains). A Bus owned by a topology snapshot or Pack returns
 * VN_IN_USE; its owner must publish the complete replacement snapshot.
 * Logic thread only. */
VnResult Vn_ReparentBus(VnBusId busId, VnBusId newParentId);

/* Destroys a bus AND its entire subtree as one structural change (one
 * topology-plan publish, applied atomically at a block boundary). Destroyed
 * slots become free and may be reused with a new generation, while the public
 * ids resolve nowhere from the moment this returns. At the swap, every live
 * voice whose dry output targeted a destroyed
 * bus migrates to the destroyed subtree root's surviving parent (pending
 * and parked plays migrate the same way at their later starts); every
 * bus->bus and voice->bus send into the destroyed subtree is disconnected;
 * installed bus effect instances are retired back to their pools (tails
 * cut, the persistent-node rule); and meter pairings, bus-scope binding
 * attachments, and bus target state on the destroyed indices are released.
 * Snapshot busPeaks keeps dense-index order with destroyed slots reading 0;
 * busCount keeps counting slots. The migration composes gains like any
 * routing change: a step at the boundary is possible when the destroyed
 * chain's gain staging differed from the surviving parent's. Refusals, all
 * with zero side effects: an unknown or already-destroyed bus is
 * VN_UNKNOWN_ID; the master bus is VN_INVALID_ARG; a Bus owned by a topology
 * snapshot or Pack is VN_IN_USE; a full topology-plan pool or command queue
 * is VN_BACKPRESSURE (retry after Vn_Update drains plan retires, or after an
 * audio block drains). Logic thread only. */
VnResult Vn_DestroyBus(VnBusId busId);

/* Values for VnNotification.type. A voice that fades to zero reports only
 * VN_NOTIFY_VOICE_STOPPED_BY_FADE, not a second ended notification. */
enum {
    VN_NOTIFY_VOICE_ENDED = 0,           /* voice finished or was cut */
    VN_NOTIFY_VOICE_STOPPED_BY_FADE = 1, /* fade-stop ramp reached zero */
    VN_NOTIFY_DEGRADATION_OCCURRED = 2,  /* a defined degradation fired
                                         * (e.g. render scratch exhausted) */
    VN_NOTIFY_VOICE_STARVED = 3,         /* a streamed voice's ring ran dry
                                         * while its source is still active;
                                         * the voice renders silence and
                                         * resumes when data arrives */

    /* Notification types reserved for playback-management surfaces. The
     * features arrive in later engine versions; the values are fixed now so
     * the ABI never renumbers. */
    VN_NOTIFY_VOICE_STOLEN = 4,          /* voice ended by steal policy to admit
                                         * a higher-priority play */
    VN_NOTIFY_VOICE_PARKED = 5,          /* voice went inaudible and released its
                                         * render slot while its timeline lives */
    VN_NOTIFY_VOICE_RETURNED = 6         /* parked voice became audible again and
                                         * reacquired a render slot */
};

/* Notification, produced on the audio thread and delivered to the
 * VnConfig callback only inside Vn_Update. */
struct VnNotification {
    uint32_t type;        /* VN_NOTIFY_* */
    uint32_t reserved0;
    VnVoiceHandle voiceHandle; /* subject voice, or 0 when not voice-bound */
    uint64_t blockTimestamp;   /* monotonic render-block index since
                                * Vn_Init; the block the notification fired in */
};

/* Pumps the engine on the calling thread: drains the notification queue
 * and invokes the VnConfig callback once per notification, then flushes
 * commits (changed Variable-driven values resolve here and travel to the
 * audio thread as commands). User callbacks run only here, never on the
 * audio thread. While suspended, these duties continue, but autonomous
 * voice-lifecycle action is deferred; audibility parking and return resume on
 * the first Vn_Update after Vn_Resume. */
VnResult Vn_Update(void);

typedef VnObjectId VnVariableId;

/* A 3D vector. The coordinate convention is the caller's; the engine only
 * ever relates positions to each other. */
typedef struct VnVec3 {
    float x;
    float y;
    float z;
} VnVec3;

/* Sets a voice's volume. volumeDb must be finite, and the reduced voice
 * volume after this API-direct contribution must convert to a finite
 * non-negative linear gain (VN_INVALID_ARG otherwise). rampMs > 0 ramps
 * linearly in the amplitude domain from the current value, completing on a
 * block boundary; rampMs == 0 snaps at the next block boundary. This writes
 * the API-direct contribution layered on the voice's authored volume — it
 * composes with, and never overwrites,
 * Variable-driven contributions. A stale handle is a benign no-op
 * returning VN_STALE_HANDLE; a full command queue returns VN_BACKPRESSURE
 * with no state change. */
VnResult Vn_SetVoiceVolume(VnVoiceHandle handle, float volumeDb,
                           uint32_t rampMs);

/* Sets one of the voice's fixed tone-stage filters. filterKind is 0 for
 * lowpass and 1 for highpass. enable01 == 0 bypasses that section (lowpass
 * fully open, highpass at 0 Hz); enable01 == 1 uses cutoffHz and q, with a
 * lowpass at or above Nyquist and a highpass at or below 0 Hz also bypassing.
 * cutoffHz and q must be finite, and an enabled q must be positive
 * (VN_INVALID_ARG otherwise). An unknown filterKind or enable01 above 1 is
 * VN_INVALID_ARG. This writes the API-direct contribution for that section's
 * cutoff and Q slots and composes with Variable-driven contributions; live
 * voices publish through the existing SetVoiceFilter command. rampMs > 0
 * ramps at the next block; rampMs == 0 snaps on a first install and otherwise
 * follows the tone-stage smoothing contract. Pending and parked voices store
 * the value without a command. Stale handle and full-queue behavior match
 * Vn_SetVoiceVolume. */
VnResult Vn_SetVoiceFilter(VnVoiceHandle handle, uint32_t filterKind,
                           float cutoffHz, float q, uint32_t enable01,
                           uint32_t rampMs);

/* Replaces the voice's independent main-output gain. Voice sends bypass
 * this gain; ordinary Volume and Variable contributions remain unchanged.
 * volumeDb must convert to a finite non-negative linear gain
 * (VN_INVALID_ARG, checked before the handle).
 * A live voice applies the change at the next audio block: rampMs == 0
 * snaps, while a positive ramp interpolates linearly in amplitude from the
 * current gain over ceil(rampMs * sampleRate / 1000) rendered frames.
 * Pending and parked voices store the value without a command, ignore
 * rampMs, and install it without a ramp at onset or park return.
 * Returns VN_OK when accepted, VN_STALE_HANDLE for a stale handle, or
 * VN_BACKPRESSURE if a live update cannot enter the command queue; refusals
 * leave the gain unchanged. An absent or failed engine returns
 * VN_NOT_INITIALIZED; an unexpected internal failure returns VN_DEVICE_ERROR. */
VnResult Vn_SetVoiceOutputVolume(VnVoiceHandle handle, float volumeDb,
                                uint32_t rampMs);

/* Sets a voice's pitch ratio (playback rate multiplier). ratio must be
 * finite, positive, and at most 64 (VN_INVALID_ARG otherwise). The change
 * takes effect at the next block boundary with playback position
 * continuity — no restart, no click. Composes with Variable-driven pitch
 * contributions on the authored ratio. Stale handle and full-queue
 * behavior match Vn_SetVoiceVolume. */
VnResult Vn_SetVoicePitch(VnVoiceHandle handle, float ratio);

/* Sets a voice's 3D emitter position. Components must be finite
 * (VN_INVALID_ARG otherwise). The position is stored on the voice at the
 * next block boundary; acoustic consequences (attenuation, panning) arrive
 * with the 3D segment. Stale handle and full-queue behavior match
 * Vn_SetVoiceVolume. A voice attached to a live emitter derives its position
 * from that emitter, so this per-voice write is refused VN_EMITTER_ATTACHED
 * while attached (destroying the emitter detaches the voice and re-enables
 * per-voice position). */
VnResult Vn_SetVoicePosition(VnVoiceHandle handle, VnVec3 position);

/* Sets the per-instance distance scale used by spatial attenuation.
 * scale must be finite and in [0.01, 100] (VN_INVALID_ARG otherwise).
 * Distance-domain calculations use pairDistance / scale; azimuth and cone
 * angle remain based on physical geometry. A value staged before onset is
 * installed without a ramp; a live change uses the existing spatial target
 * smoothing. Stale handle and full-queue behavior match Vn_SetVoiceVolume. */
VnResult Vn_SetVoiceDistanceScale(VnVoiceHandle handle, float scale);

/* Sets the per-voice spatialization mix. A finite value in [0, 1] is
 * required: 0 uses the authored 2D image, independent of geometric pan;
 * 1 uses the 3D image. The complete matrices blend without renormalization.
 * Non-spatialized voices store this value without changing their image.
 * Initial/pending/parked starts snap; live changes share one 10 ms matrix
 * transition with pan and width. Validation precedes handle lookup. */
VnResult Vn_SetVoiceSpatializationMix(VnVoiceHandle handle, float mix);

/* Sets the per-voice 2D pan. A finite value in [-1, +1] is required
 * (VN_INVALID_ARG otherwise): -1 is hard left, 0 center, +1 hard right. The
 * value is an API-direct contribution summed with any geometric pan and then
 * clamped, so a 2D voice pans directly and a 3D voice's authored offset rides
 * its geometric image. A first install, pending onset, or park return snaps,
 * while a live change uses the existing pan smoothing window. Value validation
 * precedes handle lookup. */
VnResult Vn_SetVoicePan(VnVoiceHandle handle, float pan);

/* Sets spatial spread in [0, 1]. Mono blends its front-speaker image toward
 * center with power normalization. Stereo pans its two channels to
 * (1-width)*pan +/- width. The unit-power columns share a gain of
 * 1/sqrt(1 + dot(left, right)): separated columns retain unity and
 * coincident columns use 1/sqrt(2). Width 0 is a point image and width 1
 * spans the front arc. Correlated, uncorrelated and
 * opposite-phase stereo retain their natural summation behavior; no AGC
 * is applied. Contributions combine by maximum. Initial/pending/parked
 * starts snap and live changes use the shared 10 ms matrix transition.
 * 2D voices store width without changing their image. */
VnResult Vn_SetVoiceWidth(VnVoiceHandle handle, float width);

typedef struct VnVariableResolved {
    uint32_t structSize;
    uint32_t reserved0;
    VnObjectId variableId;
    uint64_t executionFingerprint;
    uint8_t projectGuid[16];
} VnVariableResolved;

#ifdef __cplusplus
static_assert(sizeof(VnVariableResolved) == 48, "VnVariableResolved");
#endif

/* Accepts a Global write for an installed current-world Variable definition.
 * Applying it atomically removes every emitter override for this Variable,
 * even when the global value is unchanged. Other Variables are unaffected.
 * Step values must be finite exact integers within the authored range and
 * [-2^24,+2^24]; Smooth values must be finite and within the authored range.
 * Unknown Variables return VN_NO_VARIABLE. VN_OK means a durable write was
 * accepted and will reach one applied, superseded, or lifecycle termination
 * outcome. Logic thread only. */
VnResult Vn_SetValue(VnVariableId variableId, float value);
VnResult Vn_SetValueByName(const char* utf8Name, float value);
VnResult Vn_SetEmitterValueByName(VnEmitterHandle handle, const char* utf8Name, float value);

VnResult Vn_SetValueByLabelId(VnVariableId variableId, VnObjectId labelId,
                   VnEmitterHandle emitter);
VnResult Vn_SetValueByLabelName(VnVariableId variableId, const char* utf8Name,
                         VnEmitterHandle emitter);

#define VN_VARIABLE_BATCH_WRITE_CAPACITY 8

/* One row of a persistent Variable batch. reserved0 must be 0. */
typedef struct VnVariableWriteV1 {
    VnVariableId variableId;
    float value;
    uint32_t reserved0;
} VnVariableWriteV1;

/* Batch descriptor. structSize is sizeof(VnVariableBatchDescV1). writeCount
 * is 1..8. reserved fields must be 0. writes points at writeCount rows. */
typedef struct VnVariableBatchDescV1 {
    uint32_t structSize;
    uint32_t writeCount;
    uint32_t reserved0;
    uint32_t reserved1;
    const VnVariableWriteV1* writes;
} VnVariableBatchDescV1;

/* Accepted-batch ticket. Filled only when the call returns VN_OK. */
typedef struct VnVariableTicketV1 {
    uint64_t sequence;
    uint64_t worldGeneration;
    uint64_t transportGeneration;
} VnVariableTicketV1;

#ifdef __cplusplus
static_assert(sizeof(VnVariableWriteV1) == 24, "VnVariableWriteV1");
static_assert(sizeof(VnVariableBatchDescV1) == 16 + sizeof(void*), "VnVariableBatchDescV1");
static_assert(sizeof(VnVariableTicketV1) == 24, "VnVariableTicketV1");
#endif

/* Accepts one atomic Global batch of 1..8 Variable writes. Preflight failure
 * mutates nothing and issues no ticket. VN_OK means Accepted; outTicket is
 * filled only then and may be NULL. Logic thread only. */
VnResult Vn_SetValues(const VnVariableBatchDescV1* desc,
                      VnVariableTicketV1* outTicket);

/* Accepts one atomic Global Reset batch. Each row's value and reserved0 must
 * be 0. After apply, each id is the current installed default. Logic thread
 * only. */
VnResult Vn_ResetValues(const VnVariableBatchDescV1* desc,
                        VnVariableTicketV1* outTicket);

enum VnVariableWriteOutcomeV1 {
    VN_VARIABLE_OUTCOME_PENDING = 0,
    VN_VARIABLE_OUTCOME_APPLIED = 1,
    VN_VARIABLE_OUTCOME_SUPERSEDED = 2,
    VN_VARIABLE_OUTCOME_TERMINATED_WORLD = 3,
    VN_VARIABLE_OUTCOME_TERMINATED_TRANSPORT = 4,
    VN_VARIABLE_OUTCOME_TERMINATED_EMITTER = 5,
    VN_VARIABLE_OUTCOME_TERMINATED_SESSION = 6,
    VN_VARIABLE_OUTCOME_REJECTED_TARGET = 7
};

enum VnVariableTicketQueryStatusV1 {
    VN_VARIABLE_TICKET_FOUND = 0,
    VN_VARIABLE_TICKET_OUTCOME_EXPIRED = 1,
    VN_VARIABLE_TICKET_UNKNOWN = 2
};

typedef struct VnVariableTicketQueryDescV1 {
    uint32_t structSize;
    uint32_t reserved0;
    VnVariableTicketV1 ticket;
} VnVariableTicketQueryDescV1;

typedef struct VnVariableTicketQueryResultV1 {
    uint32_t structSize;
    uint32_t status;
    uint32_t outcome;
    uint32_t reserved0;
} VnVariableTicketQueryResultV1;

typedef struct VnVariableQueryDescV1 {
    uint32_t structSize;
    uint32_t flags;
    VnVariableId variableId;
    VnEmitterHandle emitter;
    uint64_t reserved0;
} VnVariableQueryDescV1;

typedef struct VnVariableQueryResultV1 {
    uint32_t structSize;
    uint32_t kind;
    uint32_t scope; /* actual value source: 0 Global, 1 Emitter override */
    uint32_t writers;
    float rangeMin;
    float rangeMax;
    float defaultValue;
    float currentValue;
    uint32_t hasEmitterOverride;
    uint32_t pendingCount;
    uint64_t executionFingerprint;
} VnVariableQueryResultV1;

#ifdef __cplusplus
static_assert(sizeof(VnVariableTicketQueryDescV1) == 32,
              "VnVariableTicketQueryDescV1");
static_assert(sizeof(VnVariableTicketQueryResultV1) == 16,
              "VnVariableTicketQueryResultV1");
static_assert(sizeof(VnVariableQueryDescV1) == 40, "VnVariableQueryDescV1");
static_assert(sizeof(VnVariableQueryResultV1) == 48, "VnVariableQueryResultV1");
#endif

/* Live Variable catalog fingerprints. structSize is 24. reserved0 must be 0. */
typedef struct VnVariableCatalogInfoV1 {
    uint32_t structSize;
    uint32_t reserved0;
    uint64_t executionCatalogFingerprint;
    uint64_t sdkCatalogFingerprint;
} VnVariableCatalogInfoV1;

#ifdef __cplusplus
static_assert(sizeof(VnVariableCatalogInfoV1) == 24,
              "VnVariableCatalogInfoV1");
#endif

/* Ticket query. VN_OK with status Found / OutcomeExpired / Unknown.
 * Pending tickets remain queryable. Retains 4096 recent ticket records and
 * 4096 evicted ticket identities; older identities return Unknown. Identity
 * comparison includes sequence, worldGeneration and transportGeneration.
 * Does not pop outcomes. Logic thread only. */
VnResult Vn_QueryVariableTicket(const VnVariableTicketQueryDescV1* desc,
                                VnVariableTicketQueryResultV1* out);

/* Definition and current-value query. emitter 0 is Global. Logic thread only. */
VnResult Vn_QueryVariable(const VnVariableQueryDescV1* desc,
                          VnVariableQueryResultV1* out);

/* Live execution and SDK catalog fingerprints. Logic thread only. Not a
 * remote-wire command. */
VnResult Vn_QueryVariableCatalog(VnVariableCatalogInfoV1* out);

/* Severity of a drained log record (VnLogFn's `level` argument). */
enum {
    VN_LOG_INFO = 0,
    VN_LOG_WARN = 1,
    VN_LOG_ERROR = 2
};

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
#define VN_MAX_CHANNELS 8

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

/* One effect type's pool statistics. effectType is the VnEffectType the row
 * describes; capacity is that type's configured pool size; activeCount is how
 * many instances are live now; highWater is the most ever live at once;
 * refusalCount is the cumulative number of pool-exhaustion refusals for the
 * type (a Set refused because the pool was full). reserved0 is zero. */
typedef struct VnEffectTypeStats {
    uint32_t effectType;   /* VnEffectType this row describes */
    uint32_t capacity;     /* configured instances for this type */
    uint32_t activeCount;  /* instances currently live */
    uint32_t highWater;    /* most instances ever live at once */
    uint32_t refusalCount; /* cumulative pool-exhaustion refusals */
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
    uint64_t noVoiceCount;        /* voice admissions refused with VN_NO_VOICE,
                                   * including capacity, policy, or controlled
                                   * logic-side allocation failures */
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
} VnSnapshot;

/* Fills the caller's snapshot from the audio-side published double buffer
 * plus logic-owned counters. Never blocks the audio thread and never
 * touches audio-mutable state outside the published buffer. NULL or a
 * structSize mismatch is VN_INVALID_ARG; no engine is
 * VN_NOT_INITIALIZED. */
VnResult Vn_CaptureSnapshot(VnSnapshot* outSnapshot);

typedef VnObjectId VnAttenuationId;

/* Values for VnAttenuationDesc.rolloff: how the built-in distance rolloff
 * maps distance to gain between minDistance and maxDistance. */
enum {
    VN_ROLLOFF_LINEAR = 0,         /* gain falls linearly to 0 at max */
    VN_ROLLOFF_INVERSE_SQUARE = 1, /* physical 1/d^2 power law */
    VN_ROLLOFF_LOGARITHMIC = 2     /* gain falls linearly per log-distance */
};

/* Attenuation override curves and binding curves (VnCurveDesc, below)
 * use cubic Bezier segments with one handle pair per segment. Evaluation
 * at or beyond the curve's
 * last x returns the last y, and before the first x the first y — the
 * existing out-of-span clamp, which keeps the final point closed. */
/* Control-point capacity of one attenuation override curve. */
#define VN_ATTENUATION_MAX_CURVE_POINTS 8

/* One override control point: x is distance (engine units), y is the
 * target's value in its own domain (dB for volume, Hz for lowpass/highpass,
 * 0..1 for width). */
typedef struct VnAttenuationCurvePoint {
    float x;
    float y;
} VnAttenuationCurvePoint;

/* Cubic Bezier handles for segment points[i] -> points[i + 1].
 * Handles use absolute coordinates. Each handle x lies within that
 * segment's endpoint interval; h1x <= h2x is not required. The ABI layout
 * is 16 bytes with 4-byte alignment. */
typedef struct VnCurveHandlePair {
    float h1x;
    float h1y;
    float h2x;
    float h2y;
} VnCurveHandlePair;

/* Optional per-target override curve inside an attenuation profile.
 * enabled = 0 hands the target to the built-in default; enabled = 1
 * evaluates these control points instead (1..VN_ATTENUATION_MAX_CURVE_POINTS
 * points with strictly increasing x and one VnCurveHandlePair per segment).
 * Values above 1 are reserved and refused (VN_INVALID_ARG). Point and handle
 * coordinates must be finite. Volume y values are dB in [-96, 0] and are
 * evaluated in linear amplitude before conversion back to dB; lowpass y must
 * be > 0; highpass y must be >= 0; width y must be in [0, 1]. Unused handle slots (i >= pointCount
 * - 1) must be bitwise +0.0f. Outside the x span the curve clamps to its
 * end values. A disabled width curve contributes zero width at every
 * distance; authored/API width contributions still combine by maximum.
 * A disabled highpass curve contributes 0 Hz; authored/API/Variable
 * highpass contributions still combine by maximum. */
typedef struct VnAttenuationCurve {
    uint32_t enabled;
    uint32_t pointCount;
    VnAttenuationCurvePoint points[VN_ATTENUATION_MAX_CURVE_POINTS];
    VnCurveHandlePair handles[VN_ATTENUATION_MAX_CURVE_POINTS - 1];
} VnAttenuationCurve;

/* Describes one attenuation profile: the parameter set mapping a voice's
 * distance (and, for the cone, its orientation) to parameter changes.
 * Distances are in the caller's world units; minDistance must be > 0 and
 * less than maxDistance. The cone block participates only when
 * coneEnabled is 1 (0 = omnidirectional; values above 1 are reserved
 * and refused); angles are in degrees measured from the emitter's
 * forward axis, 0 <= inner <= outer <= 180. */
typedef struct VnAttenuationDesc {
    uint32_t structSize;   /* set to sizeof(VnAttenuationDesc) */
    uint32_t rolloff;      /* VN_ROLLOFF_* distance rolloff choice */
    float minDistance;     /* rolloff/air mapping start; > 0 */
    float maxDistance;     /* rolloff/air mapping end; > minDistance */
    float airAbsorptionNearHz; /* lowpass cutoff at minDistance; > 0 */
    float airAbsorptionFarHz;  /* lowpass cutoff at maxDistance; > 0 */
    float proximityRadius;     /* near-field boost radius; 0 disables */
    float proximityMaxBoostDb; /* low-shelf boost at distance 0 */
    uint32_t coneEnabled;      /* 0 = omnidirectional (cone ignored) */
    float coneInnerDeg;        /* full gain inside this angle */
    float coneOuterDeg;        /* outer values at/after this angle */
    float coneOuterGainDb;     /* gain contribution at/behind outer */
    float coneOuterLowpassHz;  /* lowpass at/behind outer */
    uint32_t reserved0;        /* keeps 8-byte alignment */
    VnAttenuationCurve volumeCurve;  /* overrides rolloff; y in dB */
    VnAttenuationCurve lowpassCurve; /* overrides air absorption; y in Hz */
    VnAttenuationCurve widthCurve;   /* overrides zero width; y 0..1 */
    float proximityFrequency; /* authored near-field shelf corner in Hz */
    VnAttenuationCurve highpassCurve; /* distance highpass; y >= 0 Hz */
} VnAttenuationDesc;

/* Sets the listener's position and orientation. Components must be finite
 * (VN_INVALID_ARG otherwise); forward and up must be non-zero and not
 * parallel, because they define the panning basis (VN_INVALID_ARG
 * otherwise). Takes effect on the next Vn_Update, when spatialized voices
 * re-evaluate. Before the first call the listener sits at the origin
 * facing (0,0,-1) with up (0,1,0). */
VnResult Vn_SetListener(VnVec3 pos, VnVec3 forward, VnVec3 up);

/* Selects an optional spatial emitter as the distance reference. Zero clears
 * the selection and uses the listener position (the default). Distance-driven
 * attenuation uses the reference position; pan and cone still use the actual
 * listener. The reference's orientation and distance scale are ignored.
 * Does not retain the emitter: destruction clears a matching full handle,
 * and Init starts with no selection. A stale/non-public handle returns
 * VN_STALE_HANDLE; StateOnly returns VN_CAPABILITY_UNAVAILABLE, without
 * changing the selection. Re-selecting the same handle still validates it.
 * Consumed at spatial evaluation and start installation. Logic thread only. */
VnResult Vn_SetListenerDistanceReference(VnEmitterHandle handle);

/* Registers an attenuation profile under an id. The descriptor is copied;
 * caller memory is free to go. Refusals: null desc or structSize mismatch,
 * non-finite or out-of-contract fields, and invalid override curves are
 * VN_INVALID_ARG; an id already registered is VN_IN_USE; a full profile
 * table is VN_NO_ATTENUATION. */
VnResult Vn_RegisterAttenuation(VnAttenuationId id,
                                const VnAttenuationDesc* desc);

/* Binds a spatialized voice to a registered attenuation profile. From the
 * next Vn_Update the profile's distance-driven contributions (volume,
 * lowpass, highpass, width, near-field shelf, cone) apply to the voice; an unbound
 * 3D voice pans but does not attenuate. An unknown attenuation id is
 * VN_UNKNOWN_ID; a stale handle is a benign no-op returning
 * VN_STALE_HANDLE. Binding a 2D voice is accepted and has no acoustic
 * effect (the profile evaluates only for spatialized voices). */
VnResult Vn_BindVoiceAttenuation(VnVoiceHandle handle,
                                 VnAttenuationId attenuationId);

/* Sets a spatialized voice's emitter forward direction, consumed by the
 * directional cone. Components must be finite and the vector non-zero
 * (VN_INVALID_ARG otherwise); the engine normalizes it. Voices start
 * facing (0,0,-1). A stale handle is a benign no-op returning
 * VN_STALE_HANDLE. A voice attached to a live emitter derives its
 * orientation from that emitter, so this per-voice write is refused
 * VN_EMITTER_ATTACHED while attached. */
VnResult Vn_SetVoiceOrientation(VnVoiceHandle handle, VnVec3 forward);

/* Engine-minted generational pack handle, same scheme as voice
 * handles: low 32 bits slot index, high 32 bits generation, generation
 * increments on slot reuse. A stale handle is detected structurally and
 * every operation on it is a benign no-op returning VN_STALE_HANDLE.
 * Zero is never a valid handle. */
typedef uint64_t VnPackHandle;

/* Describes one .pack file to load. The path is UTF-8 regardless of
 * the process codepage; it is read during the call only. */
typedef struct VnPackDesc {
    uint32_t structSize;  /* set to sizeof(VnPackDesc) */
    uint32_t reserved0;   /* keeps 8-byte alignment for the pointer */
    const char* utf8Path; /* .pack file location, UTF-8 */
} VnPackDesc;

typedef struct VnPackByteSpan {
    uint32_t structSize;
    uint32_t reserved0;
    const uint8_t* bytes;
    uint64_t byteCount;
} VnPackByteSpan;

typedef VnResult (*VnPackByteProviderFn)(
    void* userData,
    uint64_t packStableId,
    const char* relativeLocatorUtf8,
    uint32_t variantKey,
    uint64_t expectedContentHash,
    VnPackByteSpan* outSpan);

typedef void (*VnPackByteReleaseFn)(
    void* userData,
    uint64_t packStableId,
    VnPackByteSpan span);

#define VN_PACK_LOAD_FLAG_PROVIDER ((uint32_t)0x00000001u)
#define VN_PACK_LOAD_FLAG_ROOT ((uint32_t)0x00000002u)
#define VN_PACK_UNLOAD_REFUSE_IF_USED ((uint32_t)0x00000001u)

typedef struct VnPackLoadDesc {
    uint32_t structSize;
    uint32_t flags; /* bit0 = provider, bit1 = containment root */
    uint64_t packStableId; /* 0 = inspect/load Init from utf8Path */
    const char* utf8Path;
    const char* utf8ContainmentRoot;
    void* providerUserData;
    VnPackByteProviderFn provide;
    VnPackByteReleaseFn release;
} VnPackLoadDesc;

#define VN_CANONICAL_PACK_SET_MEMBER_PROVIDER ((uint32_t)0x00000001u)
#define VN_CANONICAL_PACK_SET_MEMBER_ROOT ((uint32_t)0x00000002u)
#define VN_CANONICAL_PACK_SET_MEMBER_INIT ((uint32_t)2u)
#define VN_CANONICAL_PACK_SET_MEMBER_CONTENT ((uint32_t)3u)
#define VN_CANONICAL_PACK_SET_MEMBER_RESULT_RETAINED ((uint32_t)0x00000001u)
#define VN_CANONICAL_PACK_SET_MEMBER_RESULT_ADDED ((uint32_t)0x00000002u)
#define VN_CANONICAL_PACK_SET_MEMBER_RESULT_INIT ((uint32_t)0x00000004u)

typedef struct VnCanonicalPackSetMemberV1 {
    uint32_t structSize;
    uint32_t flags;
    uint64_t packStableId;
    VnPackHandle expectedHandle;
    uint32_t packKind;
    uint32_t reserved0;
    const char* utf8Path;
    const char* utf8ContainmentRoot;
    void* providerUserData;
    VnPackByteProviderFn provide;
    VnPackByteReleaseFn release;
} VnCanonicalPackSetMemberV1;

typedef struct VnCanonicalPackSetMemberResultV1 {
    uint32_t structSize;
    uint32_t flags;
    uint64_t packStableId;
    VnPackHandle handle;
    uint64_t reserved0;
} VnCanonicalPackSetMemberResultV1;

typedef struct VnCanonicalPackSetActivationDescV1 {
    uint32_t structSize;
    uint32_t flags;
    VnPackHandle expectedCurrentInitHandle;
    uint64_t baseSetRevision;
    uint64_t setRevision;
    uint32_t memberCount;
    uint32_t memberStructSize;
    const VnCanonicalPackSetMemberV1* members;
    uint32_t outputMemberCapacity;
    uint32_t outputMemberStructSize;
    VnCanonicalPackSetMemberResultV1* outputMembers;
} VnCanonicalPackSetActivationDescV1;

typedef struct VnCanonicalPackSetActivationReportV1 {
    uint32_t structSize;
    uint32_t flags;
    VnPackHandle initHandle;
    uint64_t priorSetRevision;
    uint64_t resultSetRevision;
    uint64_t activationGeneration;
    uint64_t busContentGeneration;
    uint64_t publishedBusFingerprint;
    uint64_t compileTargetProfileFingerprint;
    uint32_t memberCount;
    uint32_t reserved0;
} VnCanonicalPackSetActivationReportV1;

typedef struct VnCanonicalPackSetUnloadDescV1 {
    uint32_t structSize;
    uint32_t flags;
    VnPackHandle expectedInitHandle;
    uint64_t baseSetRevision;
    uint64_t unloadRevision;
    uint64_t reserved0;
} VnCanonicalPackSetUnloadDescV1;

VnResult Vn_SetGameplayCatalog(VnPackHandle expectedInitHandle,
                              const VnPackByteSpan* catalog);

VnResult Vn_ActivateCanonicalPackSetV1(
    const VnCanonicalPackSetActivationDescV1* desc,
    VnCanonicalPackSetActivationReportV1* outReport);
VnResult Vn_UnloadCanonicalPackSetV1(
    const VnCanonicalPackSetUnloadDescV1* desc);

/* Companion names are supplied with the captured pack, never inferred from
 * an authored-history catalog. Strings and rows are copied during the call.
 * Init supplies all Variable names and any named subset of its Labels.
 * Content supplies exactly its root Action name and the selected language
 * (empty for General). Object identity remains the authored GUID. */
typedef struct VnPackVariableNameV1 {
    VnVariableId variableId;
    const char* utf8Name;
} VnPackVariableNameV1;

typedef struct VnPackLabelNameV1 {
    VnVariableId variableId;
    VnObjectId labelId;
    const char* utf8Name;
} VnPackLabelNameV1;

typedef struct VnPackNamesV1 {
    uint32_t structSize;
    uint32_t flags;
    VnObjectId projectId;
    VnObjectId rootActionId;
    const char* utf8RootName;
    const char* utf8Language;
    uint32_t variableCount;
    uint32_t labelCount;
    const VnPackVariableNameV1* variables;
    const VnPackLabelNameV1* labels;
} VnPackNamesV1;

#define VN_PACK_MEMBER_KEEP ((uint32_t)0u)
#define VN_PACK_MEMBER_CONTENT ((uint32_t)1u)
#define VN_PACK_MEMBER_NAME_ONLY ((uint32_t)2u)

typedef struct VnCanonicalPackSetMemberV2 {
    uint32_t structSize;
    uint32_t operation;
    VnCanonicalPackSetMemberV1 source;
    const VnPackNamesV1* names;
} VnCanonicalPackSetMemberV2;

/* The complete desired set, Init first, remaining members ordered by pack ID.
 * Keep has only source identity/kind/expectedHandle, with no source or names.
 * Content and NameOnly capture the source and names; NameOnly requires the
 * bytes to match the installed content. Omitted owners are unloaded atomically.
 * V1 layouts and legacy semantics remain unchanged. A companion Init accepts
 * only V2 mutations until unloaded; legacy catalog writes cannot replace it. */
typedef struct VnCanonicalPackSetActivationDescV2 {
    uint32_t structSize;
    uint32_t flags;
    VnPackHandle expectedCurrentInitHandle;
    uint64_t baseSetRevision;
    uint64_t setRevision;
    uint32_t memberCount;
    uint32_t memberStructSize;
    const VnCanonicalPackSetMemberV2* members;
    uint32_t outputMemberCapacity;
    uint32_t outputMemberStructSize;
    VnCanonicalPackSetMemberResultV1* outputMembers;
} VnCanonicalPackSetActivationDescV2;

VnResult Vn_ActivateCanonicalPackSetV2(
    const VnCanonicalPackSetActivationDescV2* desc,
    VnCanonicalPackSetActivationReportV1* outReport);

typedef struct VnInitPackInspectReport {
    uint32_t structSize;
    uint32_t status;
    VnResult result;
    uint32_t reserved0;
    uint64_t packStableId;
    uint64_t catalogEntries;
    uint64_t catalogBytes;
    uint32_t recommendedMaxCatalogEntries;
    uint32_t recommendedMaxActions;
    uint64_t recommendedCatalogBytes;
    uint64_t recommendedResidentDefinitionBytes;
    uint64_t requiredCapabilityMask;
    uint64_t semanticFingerprint;
} VnInitPackInspectReport;

/* Legal before Vn_Init. Does not create an engine, install partitions, or
 * retain bytes after return. */
VnResult Vn_InspectInitPack(const VnPackLoadDesc* desc,
                            VnInitPackInspectReport* outReport);

#define VN_PACK_PREVIEW_UNSUPPORTED ((uint32_t)0u)
#define VN_PACK_PREVIEW_SOUND ((uint32_t)1u)
#define VN_PACK_PREVIEW_RANDOM_SOUNDS ((uint32_t)2u)
#define VN_PACK_OCCURRENCE_FIXED_GAIN ((uint32_t)1u)
#define VN_PACK_OCCURRENCE_DURATION ((uint32_t)2u)

typedef struct VnPackVariableFactV1 {
    VnVariableId variableId;
    uint32_t kind;
    uint32_t writers;
    float rangeMin;
    float rangeMax;
    float defaultValue;
    uint32_t reserved0;
    uint64_t executionFingerprint;
    uint64_t labelMappingFingerprint;
} VnPackVariableFactV1;

typedef struct VnPackLabelFactV1 {
    VnVariableId variableId;
    VnObjectId labelId;
    int32_t anchor;
    uint32_t reserved0;
} VnPackLabelFactV1;

/* Facts about the converted Pack media, independent of preview classification.
 * Source waveform rate/trim coordinates come from companion JSON and VKD. */
typedef struct VnPackMediaFactV1 {
    VnSoundId soundId;
    uint64_t mediaChecksum;
    uint64_t frameCount;
    uint32_t sampleRate;
    uint32_t channelCount;
} VnPackMediaFactV1;

typedef struct VnPackOccurrenceFactV1 {
    VnObjectId rootPlayableId;
    VnSoundId soundId;
    uint32_t rootPlayableKind;
    uint32_t childOrdinal; /* UINT32_MAX for a direct Sound. */
    uint64_t rootProgramFingerprint;
    uint64_t soundProgramFingerprint;
    uint64_t occurrenceFingerprint;
    uint64_t mediaChecksum;
    uint64_t mediaFrameCount;
    uint32_t sampleRate;
    uint32_t channelCount;
    uint32_t flags;
    uint32_t reserved0;
    float fixedGainDb;
    float fixedGainLinear;
    double durationSeconds;
} VnPackOccurrenceFactV1;

typedef struct VnPackInspectDescV1 {
    uint32_t structSize;
    uint32_t flags;
    VnPackByteSpan pack;
    VnPackByteSpan init; /* Required for Content; empty for Init. */
    const VnPackNamesV1* names;
    uint64_t maxInputBytes;
    uint32_t variableCapacity;
    uint32_t labelCapacity;
    uint32_t occurrenceCapacity;
    uint32_t mediaCapacity;
    VnPackVariableFactV1* variables;
    VnPackLabelFactV1* labels;
    VnPackOccurrenceFactV1* occurrences;
    VnPackMediaFactV1* media;
} VnPackInspectDescV1;

typedef struct VnPackInspectReportV1 {
    uint32_t structSize;
    uint32_t packKind;
    VnObjectId projectId;
    VnObjectId rootActionId;
    uint64_t packStableId;
    uint64_t initPackStableId;
    uint64_t contentRevision;
    uint64_t contentHash;
    uint64_t initContentHash;
    uint64_t executionFingerprint;
    uint32_t variableCount;
    uint32_t labelCount;
    uint32_t occurrenceCount;
    uint32_t previewKind;
    uint32_t mediaCount;
    uint32_t reserved0;
} VnPackInspectReportV1;

/* Legal before Init; borrows immutable inputs only for this call. Uses the
 * shipping decoder and property fold. No engine or media installation occurs.
 * A zero names.projectId discovers the actual project from the Pack; a nonzero
 * value must match. Activation always requires an explicit project identity.
 * Zero capacities with null arrays query required counts. Otherwise every
 * capacity must suffice; VN_PACK_RESIDENT_CAPACITY writes only the report.
 * Unsupported preview is a successful inspection with zero occurrences.
 * Fixed gain is the authored source/main-output chain, not a mixed DSP result.
 * Only flagged duration/gain fields are known. No decoder pointers escape. */
VnResult Vn_InspectCanonicalPackV1(const VnPackInspectDescV1* desc,
                                  VnPackInspectReportV1* outReport);

typedef uint32_t VnCurrentWorldLoadStatus;

enum {
    VN_CURRENT_WORLD_LOAD_OK = 0,
    VN_CURRENT_WORLD_LOAD_INVALID_ARGUMENT = 1,
    VN_CURRENT_WORLD_LOAD_IO_ERROR = 2,
    VN_CURRENT_WORLD_LOAD_TOO_LARGE = 3,
    VN_CURRENT_WORLD_LOAD_BAD_MAGIC = 4,
    VN_CURRENT_WORLD_LOAD_UNSUPPORTED_ENVELOPE = 5,
    VN_CURRENT_WORLD_LOAD_UNSUPPORTED_BODY = 6,
    VN_CURRENT_WORLD_LOAD_TRUNCATED = 7,
    VN_CURRENT_WORLD_LOAD_SIZE_MISMATCH = 8,
    VN_CURRENT_WORLD_LOAD_CHECKSUM_MISMATCH = 9,
    VN_CURRENT_WORLD_LOAD_DECODE_FAILED = 10,
    VN_CURRENT_WORLD_LOAD_SEMANTIC_INVALID = 11,
    VN_CURRENT_WORLD_LOAD_CAPABILITY_UNAVAILABLE = 12,
    VN_CURRENT_WORLD_LOAD_CAPACITY = 13,
    VN_CURRENT_WORLD_LOAD_BACKPRESSURE = 14,
    VN_CURRENT_WORLD_LOAD_RUNTIME_RECREATE_REQUIRED = 15,
    VN_CURRENT_WORLD_LOAD_NOT_INITIALIZED = 16
};

typedef struct VnCurrentWorldLoadDesc {
    uint32_t structSize;
    uint32_t flags;
    const char* utf8Path;
} VnCurrentWorldLoadDesc;

typedef struct VnCurrentWorldLoadReport {
    uint32_t structSize;
    VnCurrentWorldLoadStatus status;
    VnResult result;
    uint32_t reserved0;
    uint64_t priorGeneration;
    uint64_t resultGeneration;
    uint64_t runtimeSemanticFingerprint;
    uint64_t capabilityMask;
    uint64_t bodyBytes;
    uint64_t bodyChecksum;
} VnCurrentWorldLoadReport;

/* Studio preview DirectWorld envelope (VNCWORLD). The body is the same
 * playback-program bytes as CanonicalDefinitionPartition, with the same
 * shipping bodyVintage. Games load .pack; this is not a third disk
 * contract. */
VnResult Vn_LoadCurrentWorld(const VnCurrentWorldLoadDesc* desc,
                             VnCurrentWorldLoadReport* outReport);

/* Loads a .pack container and registers every entry it carries, in
 * reference mode — the pack's memory stays pinned inside the engine until
 * unload. The whole load is validated first (header sanity, TOC checksum,
 * every blob checksum, offset bounds) and is all-or-nothing: an invalid or
 * corrupted container refuses with VN_BAD_PACK and an entry id that is
 * already registered refuses with VN_IN_USE, in both cases leaving the
 * registry untouched. A full pack table refuses with VN_NO_PACK. A pack whose
 * activation would need more control commands at once than the configured
 * command queue or per-block command ceiling can take refuses with
 * VN_INVALID_CONFIG: that batch applies indivisibly, so no amount of waiting
 * makes it fit, and the configuration has to change instead. Packs are
 * a bulk path over the same registry as Vn_RegisterPCM — entries play
 * through the ordinary VnSoundId surface, and individual registration
 * remains fully supported alongside. Reading the file happens on the
 * calling (logic) thread; the audio thread is never involved. */
/* desc may be a VnPackDesc or a VnPackLoadDesc; structSize selects the vintage.
 * A VnPackDesc cannot activate Init / CanonicalContent Packs. Additive
 * Vn_ActivateCanonicalPackSetV1 is the Init set path; this load still accepts
 * current-vintage Init / CanonicalContent. A canonical pack whose playback-program
 * bodyVintage is not the current write table refuses with VN_PACK_INCOMPATIBLE;
 * payload damage is VN_BAD_PACK. */
VnResult Vn_LoadPack(const VnPackDesc* desc,
                          VnPackHandle* outHandle);

/* Unloads a loaded pack and releases its current media registrations. The
 * legacy call permits live PCM users: those registrations leave the lookup
 * table immediately while the old generation remains pinned until its voices
 * drain. An entry id the caller has since unregistered individually — and
 * possibly re-registered with content of their own — is no longer the pack's;
 * it neither blocks the unload nor is removed by it. A stale handle is a
 * benign no-op returning VN_STALE_HANDLE. */
VnResult Vn_UnloadPack(VnPackHandle handle);
/* flags must contain only VN_PACK_UNLOAD_REFUSE_IF_USED. With that flag, an
 * anchored revision or any still-used owned PCM returns VN_IN_USE without
 * changing the current world. Unknown flags return VN_INVALID_ARG. */
VnResult Vn_UnloadPackEx(VnPackHandle handle, uint32_t flags);

/* Bypasses or re-enables one of a voice's insert-effect slots. slot indexes
 * the voice's chain (0-based, below the configured fxSlotsPerVoice);
 * bypass01 is 0 to run the slot (wet) or 1 to bypass it (dry). The change
 * takes effect at the next block boundary as a one-block linear wet/dry
 * crossfade, so toggling never clicks. An empty slot toggles nothing
 * audible but is still accepted. bypass01 above 1, or a slot at or beyond
 * fxSlotsPerVoice, is VN_INVALID_ARG; a stale handle is a benign no-op
 * returning VN_STALE_HANDLE; a full command queue returns VN_BACKPRESSURE
 * with no state change. */
VnResult Vn_SetVoiceEffectBypass(VnVoiceHandle handle, uint32_t slot,
                                 uint32_t bypass01);

/* Built-in insert-effect types (append-only; values are ABI-fixed and never
 * renumbered). Each type has its own descriptor struct and its own private,
 * 0-based parameter index space (never the base spec's global parameter-target
 * IDs). */
typedef uint32_t VnEffectType;
enum {
    VN_EFFECT_FEEDBACK_DELAY = 0,  /* soft-saturated feedback delay */
    VN_EFFECT_THREE_BAND_TONE = 1, /* low shelf + peaking + high shelf */
    VN_EFFECT_GAIN = 2,            /* memoryless per-channel linear gain (an
                                    * insert-effect chain stage, distinct from a
                                    * voice's fixed Voice Gain and a bus's Bus
                                    * Volume fader) */
    VN_EFFECT_LIMITER = 3,         /* sample-peak, zero-lookahead, linked-channel
                                    * hard-Ceiling limiter */
    VN_EFFECT_COMPRESSOR = 4       /* feed-forward linked-channel compressor */
};

VnResult Vn_ResetVoiceEffect(VnVoiceHandle handle, uint32_t effectSlot,
                             VnEffectType expectedEffectType);

/* Feedback-delay parameter indices (for Vn_SetVoiceEffectParam):
 *   0 delay time in ms   (0, 1000]      1 feedback        [0, 1)
 *   2 wet level          [0, 16]        3 dry level       [0, 16]
 * Three-band-tone parameter indices:
 *   0 low-shelf gain dB  [-48, 48]      1 low-shelf freq Hz  (0, Nyquist)
 *   2 peak gain dB       [-48, 48]      3 peak freq Hz       (0, Nyquist)
 *   4 peak Q             (0, 100]       5 high-shelf gain dB [-48, 48]
 *   6 high-shelf freq Hz (0, Nyquist)
 * Gain parameter indices:
 *   0 gain dB            [-96, 24] (0 dB is exact unity, bit-transparent)
 * Limiter parameter indices:
 *   0 Ceiling dB         [-24, 0]       1 Release ms      [1, 1000]
 * Compressor parameter indices:
 *   0 threshold dB       [-96, 0]       1 ratio           [1, 20]
 *   2 attack ms          [0.1, 200]     3 release ms      [5, 3000]
 *   4 knee dB            [0, 24]        5 makeup dB       [-24, 24] */

/* Base descriptor header. Every concrete effect descriptor begins with these
 * two fields, so Vn_SetVoiceEffect reads structSize through this base pointer
 * and then interprets the descriptor per the VnEffectType argument. This shape
 * accommodates future descriptor types that carry additional data (e.g. a
 * plugin host type with path/id strings) without changing this call. */
typedef struct VnEffectDesc {
    uint32_t structSize; /* set to sizeof(the concrete descriptor) */
    uint32_t reserved0;  /* keeps 8-byte alignment for concrete fields */
    VnObjectId effectId;
} VnEffectDesc;

/* Descriptor for VN_EFFECT_FEEDBACK_DELAY. delayMs must be in (0, 1000];
 * feedback must be in [0, 1) (a loop at or above unity would never decay);
 * wet and dry are linear levels in [0, 16]. Non-finite or out-of-range fields
 * are VN_INVALID_ARG. */
typedef struct VnFeedbackDelayDesc {
    uint32_t structSize; /* set to sizeof(VnFeedbackDelayDesc) */
    uint32_t reserved0;
    VnObjectId effectId;
    float delayMs;
    float feedback;
    float wet;
    float dry;
} VnFeedbackDelayDesc;

/* Descriptor for VN_EFFECT_THREE_BAND_TONE: an RBJ low shelf, peaking band,
 * and high shelf in series. Gains are in dB within [-48, 48]; frequencies are
 * in Hz within (0, Nyquist); the peak Q is in (0, 100]. Non-finite or
 * out-of-range fields are VN_INVALID_ARG. */
typedef struct VnThreeBandToneDesc {
    uint32_t structSize; /* set to sizeof(VnThreeBandToneDesc) */
    uint32_t reserved0;
    VnObjectId effectId;
    float lowShelfGainDb;
    float lowShelfFreqHz;
    float peakGainDb;
    float peakFreqHz;
    float peakQ;
    float highShelfGainDb;
    float highShelfFreqHz;
} VnThreeBandToneDesc;

/* Descriptor for VN_EFFECT_GAIN: one memoryless linear gain multiplying every
 * processed channel. gainDb is in dB within [-96, 24]; 0 dB is exact unity and
 * passes the signal through bit-for-bit (a neutral fast path, no multiply).
 * Non-finite or out-of-range fields are VN_INVALID_ARG. This is the Gain insert
 * effect (a chain stage), distinct from a voice's fixed Voice Gain and a bus's
 * Bus Volume fader. */
typedef struct VnGainDesc {
    uint32_t structSize; /* set to sizeof(VnGainDesc) */
    uint32_t reserved0;
    VnObjectId effectId;
    float gainDb;
} VnGainDesc;

/* Descriptor for VN_EFFECT_LIMITER. ceilingDb is the inclusive hard-Ceiling
 * range [-24, 0] dB (default 0 dB). releaseMs is the inclusive release-time
 * range [1, 1000] ms (default 100 ms). The limiter is sample-peak,
 * feed-forward, linked-channel,
 * zero-lookahead, zero-added-latency, and zero-Tail. Non-finite or
 * out-of-range fields are VN_INVALID_ARG. */
typedef struct VnLimiterDesc {
    uint32_t structSize; /* set to sizeof(VnLimiterDesc) */
    uint32_t reserved0;
    VnObjectId effectId;
    float ceilingDb;
    float releaseMs;
} VnLimiterDesc;

/* Descriptor for VN_EFFECT_COMPRESSOR. The six fields use the closed ranges
 * listed above. The detector is feed-forward and linked-channel; an instance
 * with no installed external detector route uses its own input. Parameter
 * changes use an effect-owned 10 ms transition. Non-finite or out-of-range
 * fields are VN_INVALID_ARG. */
typedef struct VnCompressorDesc {
    uint32_t structSize; /* set to sizeof(VnCompressorDesc) */
    uint32_t reserved0;
    VnObjectId effectId;
    float thresholdDb;
    float ratio;
    float attackMs;
    float releaseMs;
    float kneeDb;
    float makeupDb;
} VnCompressorDesc;

/* Effect capability row vocabularies. Values mirror the generated built-in
 * effect registry and are append-only. hostScopeMask is a bitmask; flags is a
 * bitmask. */
typedef uint32_t VnEffectHostScopeFlags;
enum {
    VN_EFFECT_HOST_SCOPE_VOICE = 1u << 0,
    VN_EFFECT_HOST_SCOPE_BUS = 1u << 1
};

typedef uint32_t VnEffectParameterUnit;
enum {
    VN_EFFECT_PARAMETER_UNIT_DB = 0,
    VN_EFFECT_PARAMETER_UNIT_LINEAR = 1,
    VN_EFFECT_PARAMETER_UNIT_HZ = 2,
    VN_EFFECT_PARAMETER_UNIT_Q = 3,
    VN_EFFECT_PARAMETER_UNIT_MS = 4,
    VN_EFFECT_PARAMETER_UNIT_RATIO = 5
};

typedef uint32_t VnEffectParameterCombine;
enum {
    VN_EFFECT_PARAMETER_COMBINE_ADD_DB = 0,
    VN_EFFECT_PARAMETER_COMBINE_MULTIPLY = 1,
    VN_EFFECT_PARAMETER_COMBINE_ADD_LOG_HZ = 2,
    VN_EFFECT_PARAMETER_COMBINE_ADD_LOG_Q = 3,
    VN_EFFECT_PARAMETER_COMBINE_NONE = 4,
    VN_EFFECT_PARAMETER_COMBINE_ADD_LOG_TIME = 5
};

typedef uint32_t VnEffectParameterFlags;
enum {
    VN_EFFECT_PARAMETER_BINDABLE = 1u << 0,
    VN_EFFECT_PARAMETER_SUPPORTS_TRANSITION = 1u << 1
};

typedef uint32_t VnDynamicSignalForm;
enum {
    VN_DYNAMIC_SIGNAL_CONTROL = 1u << 0,
    VN_DYNAMIC_SIGNAL_RAW_AUDIO = 1u << 1
};

typedef uint32_t VnDynamicSourceKind;
enum {
    VN_DYNAMIC_SOURCE_VARIABLE = 1u << 0,
    VN_DYNAMIC_SOURCE_BUS = 1u << 1,
    VN_DYNAMIC_SOURCE_VOICE = 1u << 2
};

typedef uint32_t VnDynamicTap;
enum {
    VN_DYNAMIC_TAP_BUS_POST_EFFECTS = 0,
    VN_DYNAMIC_TAP_BUS_PRE_FADER = 1,
    VN_DYNAMIC_TAP_BUS_POST_FADER = 2,
    VN_DYNAMIC_TAP_VOICE_POST_GAIN_PRE_SPATIAL = 3
};

typedef uint32_t VnDynamicCapabilityFlags;
enum {
    VN_DYNAMIC_CAP_CONTROL_USES_BINDING = 1u << 0,
    VN_DYNAMIC_CAP_RAW_EFFECT_LOCAL = 1u << 1,
    VN_DYNAMIC_CAP_PACK_CONTROL = 1u << 2,
    VN_DYNAMIC_CAP_PACK_RAW_BUS = 1u << 3,
    VN_DYNAMIC_CAP_RAW_VOICE_LIVE_ONLY = 1u << 4,
    VN_DYNAMIC_CAP_EFFECT_RESET = 1u << 5,
    VN_DYNAMIC_CAP_RAW_STALE_IS_SILENT = 1u << 6,
    VN_DYNAMIC_CAP_RAW_ATOMIC_PUBLICATION = 1u << 7,
    VN_DYNAMIC_CAP_RAW_REFUSES_CYCLE_ORDER_CAPACITY = 1u << 8,
    VN_DYNAMIC_CAP_RAW_RESET_RETAINS_ROUTE = 1u << 9
};

typedef uint32_t VnDynamicParameterFlags;
enum { VN_DYNAMIC_PARAM_CONTROL_SAFE = 1u << 0 };

typedef uint32_t VnDynamicRouteStateFlags;
enum {
    VN_DYNAMIC_ROUTE_AUTHORED = 1u << 0,
    VN_DYNAMIC_ROUTE_ACTIVE = 1u << 1,
    VN_DYNAMIC_ROUTE_STALE = 1u << 2,
    VN_DYNAMIC_ROUTE_FIRST_BLOCK = 1u << 3,
    VN_DYNAMIC_ROUTE_PACK_OWNED = 1u << 4,
    VN_DYNAMIC_ROUTE_PENDING = 1u << 5,
    VN_DYNAMIC_ROUTE_SOURCE_SILENT = 1u << 6
};

typedef uint32_t VnDynamicRouteInactiveReason;
enum {
    VN_DYNAMIC_ROUTE_INACTIVE_NONE = 0,
    VN_DYNAMIC_ROUTE_INACTIVE_SOURCE_STALE = 1,
    VN_DYNAMIC_ROUTE_INACTIVE_FIRST_BLOCK = 2,
    VN_DYNAMIC_ROUTE_INACTIVE_TAP_GENERATION = 3,
    VN_DYNAMIC_ROUTE_INACTIVE_TARGET_STALE = 4,
    VN_DYNAMIC_ROUTE_INACTIVE_PENDING = 5,
    VN_DYNAMIC_ROUTE_INACTIVE_NO_AUDIO_STATUS = 6
};

typedef uint32_t VnDynamicRouteOwner;
enum {
    VN_DYNAMIC_ROUTE_OWNER_CALLER = 0,
    VN_DYNAMIC_ROUTE_OWNER_PACK = 1
};

typedef uint32_t VnDynamicRawCombine;
enum {
    VN_DYNAMIC_RAW_COMBINE_NONE = 0,
    VN_DYNAMIC_RAW_COMBINE_MAX_MAGNITUDE = 1
};

typedef uint32_t VnDynamicChannelReduction;
enum {
    VN_DYNAMIC_CHANNEL_REDUCTION_NONE = 0,
    VN_DYNAMIC_CHANNEL_REDUCTION_MAX_ABSOLUTE = 1
};

typedef uint32_t VnDynamicUpdateCadence;
enum {
    VN_DYNAMIC_CADENCE_UPDATE = 0,
    VN_DYNAMIC_CADENCE_AUDIO_FRAME = 1
};

typedef uint32_t VnDynamicSmoothingOwner;
enum {
    VN_DYNAMIC_SMOOTHING_NONE = 0,
    VN_DYNAMIC_SMOOTHING_TARGET = 1,
    VN_DYNAMIC_SMOOTHING_EFFECT_INTERNAL = 2
};

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

typedef struct VnEffectDynamicRouteDesc {
    uint32_t structSize;
    uint32_t reserved0;
    uint32_t signalForm;
    uint32_t sourceKind;
    uint32_t tap;
    uint32_t reserved1;
    VnObjectId sourceId;
    uint64_t sourceVoiceHandle;
    float levelDb;
    uint32_t expectedEffectType;
} VnEffectDynamicRouteDesc;

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

/* Installs a built-in effect into one of a voice's insert-effect slots. The
 * instance is taken from the effect type's preallocated pool and configured on
 * the calling (logic) thread, then handed to the audio thread, which activates
 * it at the next block boundary; the audio thread never constructs or frees an
 * effect. Setting a slot that already holds an effect replaces it in place
 * (the old instance is reclaimed). `type` selects both the behavior and the
 * concrete descriptor the base pointer must point to. Refusals: a null or
 * wrong-size descriptor, an unknown type, an out-of-range parameter, or a slot
 * at or beyond fxSlotsPerVoice are VN_INVALID_ARG; an exhausted effect pool is
 * VN_NO_EFFECT; a stale handle is a benign no-op returning VN_STALE_HANDLE; a
 * full command queue returns VN_BACKPRESSURE with no side effects (no instance
 * is taken from the pool). */
VnResult Vn_SetVoiceEffect(VnVoiceHandle handle, uint32_t slot,
                           VnEffectType type, const VnEffectDesc* desc);

/* Removes any effect from a voice's slot at the next block boundary. The
 * cleared effect's tail is cut (not drained); the slot's bypass target is kept
 * and any in-progress bypass ramp settles immediately, so the slot then
 * behaves exactly like one that never held an effect. Clearing an already-empty
 * slot is accepted. A slot at or beyond fxSlotsPerVoice is VN_INVALID_ARG; a
 * stale handle is a benign no-op returning VN_STALE_HANDLE; a full command
 * queue returns VN_BACKPRESSURE with no state change. */
VnResult Vn_ClearVoiceEffect(VnVoiceHandle handle, uint32_t slot);

/* Updates one parameter of the effect in a voice's slot. paramIndex is the
 * effect type's private 0-based index (see the tables above); value is checked
 * against that parameter's range for the installed type. Refusals: a slot at
 * or beyond fxSlotsPerVoice, an empty slot, an unknown paramIndex, or a
 * non-finite or out-of-range value are VN_INVALID_ARG. Parameters carrying
 * VN_EFFECT_PARAMETER_SUPPORTS_TRANSITION use the effect-owned 10 ms
 * transition; other parameters change at the block boundary. A stale handle is a
 * benign no-op returning VN_STALE_HANDLE; a full command queue returns
 * VN_BACKPRESSURE with no state change. */
VnResult Vn_SetVoiceEffectParam(VnVoiceHandle handle, uint32_t slot,
                                uint32_t paramIndex, float value);

/* Registers a .stream file for streamed playback under a sound id. The
 * path is UTF-8 regardless of the process codepage. The file is opened and
 * validated (header sanity, TOC + chunk-checksum-table checksum) by the
 * streaming thread during this call — the call blocks until that check
 * completes, and the file stays open until unregistration. Entries play
 * through the internal raw-Voice start seam: starting a streamed id claims
 * a voice slot and a stream slot, holds the voice silent (PREPARING) until
 * the prebuffer threshold is met, then sounds. Refusals: a null path is
 * VN_INVALID_ARG; an id already registered (PCM or stream) is VN_IN_USE;
 * an unreadable, malformed, or checksum-failing file is VN_BAD_STREAM_FILE.
 * A refused call has no side effects. VN_DEVICE_ERROR means the streaming
 * thread itself has stopped, so no stream request can be served: the condition
 * is the subsystem, not this file, and retrying another path will not clear it.
 * Voices that were streaming end, and further stream calls keep returning it
 * for the life of the engine. */
VnResult Vn_RegisterStreamFile(VnSoundId soundId, const char* utf8Path);

/* Removes a registered stream file and closes it. Refused with VN_IN_USE
 * while any voice is playing it; VN_UNKNOWN_ID when the id names no
 * registered stream; VN_DEVICE_ERROR when the streaming thread has stopped
 * (see Vn_RegisterStreamFile). */
VnResult Vn_UnregisterStreamFile(VnSoundId soundId);

/* Creates an emitter — a game object whose position and orientation many
 * voices can follow, and which carries a small local Variable table. The
 * engine mints a generational handle into *outHandle (never 0). A voice
 * attaches by naming the handle in VnPlayDesc.emitterHandle; thereafter the
 * voice takes the emitter's pose each update and resolves its Variable reads
 * at emitter scope. Refusals: a null pointer is VN_INVALID_ARG; an exhausted
 * emitter table (maxEmitters) is VN_NO_EMITTER. Logic thread only. */
VnResult Vn_CreateEmitter(VnEmitterHandle* outHandle);

#define VN_EMITTER_CREATE_STATE_ONLY 1u

typedef struct VnEmitterCreateDescV1 {
    uint32_t structSize;
    uint32_t flags;
    uint64_t reserved0;
} VnEmitterCreateDescV1;

/* Creates a spatial emitter (flags=0) or a StateOnly emitter, which owns
 * selection/variable state without a pose. StateOnly voices retain their own
 * authored spatial behavior and accept voice pose setters. Emitter pose setters
 * return VN_CAPABILITY_UNAVAILABLE. Both kinds use the public maxEmitters quota.
 * Unknown flags, a nonzero reserved field, or an invalid size are VN_INVALID_ARG.
 * On failure a non-null outHandle is zero. Logic thread only. */
VnResult Vn_CreateEmitterEx(const VnEmitterCreateDescV1* desc,
                          VnEmitterHandle* outHandle);

/* Destroys an emitter and invalidates its handle. Voices attached to it are
 * detached, not stopped: each keeps playing with its last emitter-derived
 * position/orientation frozen and becomes individually addressable again
 * (per-voice position/orientation writes are accepted after this), and its
 * Variable reads fall back to global scope. A stale handle is a benign no-op
 * returning VN_STALE_HANDLE. To stop the attached voices instead, call
 * Vn_StopEmitter before destroying. Logic thread only. */
VnResult Vn_DestroyEmitter(VnEmitterHandle handle);

/* Sets an emitter's world position. Components must be finite
 * (VN_INVALID_ARG otherwise). Attached voices pick it up at the next
 * Vn_Update. A stale handle is a benign no-op returning VN_STALE_HANDLE.
 * Logic thread only. */
VnResult Vn_SetEmitterPosition(VnEmitterHandle handle, VnVec3 position);

/* Sets an emitter's forward orientation, used by attached voices' directional
 * cones. Components must be finite and the vector non-zero (VN_INVALID_ARG
 * otherwise); the engine normalizes it. A stale handle is a benign no-op
 * returning VN_STALE_HANDLE. Logic thread only. */
VnResult Vn_SetEmitterOrientation(VnEmitterHandle handle, VnVec3 forward);

/* Sets the emitter's distance scale (default 1). Must be positive and finite
 * (VN_INVALID_ARG otherwise). Distance attenuation uses source-to-reference distance
 * divided by this scale and the voice's distance scale; pan and cone angles
 * remain physical. Attached voices observe it at their next spatial sample.
 * VN_STALE_HANDLE for a stale handle; VN_CAPABILITY_UNAVAILABLE for a
 * StateOnly emitter. Refused calls leave state unchanged. Logic thread only. */
VnResult Vn_SetEmitterDistanceScale(VnEmitterHandle handle, float scale);

/* Stops every voice currently attached to the emitter — live voices fade over
 * fadeMs (0 = immediate cut) and voices still awaiting a render slot are
 * cancelled — leaving the emitter itself intact for reuse. A stale handle is a
 * benign no-op returning VN_STALE_HANDLE; VN_BACKPRESSURE if the command queue
 * cannot currently admit the stops (no voice is stopped — retry after a pump),
 * or VN_NO_VOICE if the complete stop batch exceeds a fixed queue or per-quantum
 * capacity. Logic thread only. */
VnResult Vn_StopEmitter(VnEmitterHandle handle, uint32_t fadeMs);

/* Accepts an emitter-local override write for an installed current-world
 * Variable. The same Step/Smooth authored-range rules as Vn_SetValue apply.
 * Voices attached to this emitter resolve the override before the Global bank;
 * unattached voices and other emitters are unaffected. A stale handle returns
 * VN_STALE_HANDLE; an unknown definition or exhausted durable row capacity
 * returns VN_NO_VARIABLE. VN_OK means exactly one terminal outcome will follow.
 * Logic thread only. */
VnResult Vn_SetEmitterValue(VnEmitterHandle handle, VnVariableId variableId,
                            float value);

/* Atomic emitter-local override batch. Same rules as Vn_SetValues, applied to
 * one live emitter. Logic thread only. */
VnResult Vn_SetEmitterValues(VnEmitterHandle emitter,
                             const VnVariableBatchDescV1* desc,
                             VnVariableTicketV1* outTicket);

/* Action objects are authored in the Current World and fired by stable id
 * against an optional emitter. Logic thread only; the audio thread never sees
 * an Action program. */

/* Caller-minted Action id (asset-ID class). */
typedef VnObjectId VnActionId;

typedef uint64_t VnActionFiringHandle;
typedef uint64_t VnActionSeekTicket;

typedef uint32_t VnActionSeekMode;
enum {
    VN_ACTION_SEEK_ABSOLUTE = 0,
    VN_ACTION_SEEK_RELATIVE = 1,
};

typedef uint32_t VnActionSeekState;
enum {
    VN_ACTION_SEEK_ACCEPTED = 0,
    VN_ACTION_SEEK_APPLIED = 1,
    VN_ACTION_SEEK_CANCELLED = 2,
};

typedef struct VnActionSeekDescV1 {
    uint32_t structSize;
    uint32_t flags;
    VnActionFiringHandle firing;
    double positionMs;
    VnActionSeekMode mode;
    uint32_t reserved0;
} VnActionSeekDescV1;

typedef struct VnActionSeekInfoV1 {
    uint32_t structSize;
    VnActionSeekState state;
    VnResult result;
    uint32_t reserved0;
    VnActionFiringHandle firing;
    uint32_t targetCount;
    uint32_t appliedCount;
    uint32_t cancelledCount;
    uint32_t reserved1;
    uint64_t acceptedLogicSequence;
    uint64_t appliedAudioBlockSequence;
    uint64_t minimumAppliedSourceFrame;
    uint64_t maximumAppliedSourceFrame;
} VnActionSeekInfoV1;

typedef uint32_t VnActionOpcode;
enum {
    VN_ACTION_OPCODE_PLAY = 0,
    VN_ACTION_OPCODE_STOP = 1,
    VN_ACTION_OPCODE_POST_ACTION = 2,
    VN_ACTION_OPCODE_SET_VARIABLE = 3,
    VN_ACTION_OPCODE_RESET = 4,
};

typedef uint32_t VnActionTargetKind;
enum {
    VN_ACTION_TARGET_SOUND = 0,
    VN_ACTION_TARGET_CONTAINER = 1,
    VN_ACTION_TARGET_TRACK = 2,
    VN_ACTION_TARGET_SEGMENT = 3,
    VN_ACTION_TARGET_ACTION = 4,
    VN_ACTION_TARGET_VARIABLE = 5,
    VN_ACTION_TARGET_BUS = 6,
};

typedef uint32_t VnActionFiringState;
enum {
    VN_ACTION_FIRING_ACTIVE = 0,
    VN_ACTION_FIRING_STOPPING = 1,
    VN_ACTION_FIRING_TERMINAL_RETAINED = 2,
};

typedef uint32_t VnActionFiringOutcome;
enum {
    VN_ACTION_OUTCOME_PENDING = 0,
    VN_ACTION_OUTCOME_SUCCEEDED = 1,
    VN_ACTION_OUTCOME_SUCCEEDED_WITH_STEP_FAILURE = 2,
    VN_ACTION_OUTCOME_FAILED = 3,
    VN_ACTION_OUTCOME_CANCELLED = 4,
};

typedef uint32_t VnActionStepState;
enum {
    VN_ACTION_STEP_PENDING = 0,
    VN_ACTION_STEP_ACTIVE = 1,
    VN_ACTION_STEP_SUCCEEDED = 2,
    VN_ACTION_STEP_NO_OP = 3,
    VN_ACTION_STEP_SUPPRESSED = 4,
    VN_ACTION_STEP_FAILED = 5,
    VN_ACTION_STEP_CANCELLED = 6,
    VN_ACTION_STEP_AWAITING_MATERIALIZATION = 7,
};

typedef uint32_t VnActionTerminalReason;
enum {
    VN_ACTION_TERMINAL_NONE = 0,
    VN_ACTION_TERMINAL_NATURAL = 1,
    VN_ACTION_TERMINAL_STOP_REQUESTED = 2,
    VN_ACTION_TERMINAL_WORLD_REPLACED = 3,
    VN_ACTION_TERMINAL_TRANSPORT_RESET = 4,
    VN_ACTION_TERMINAL_TARGET_GONE = 5,
    VN_ACTION_TERMINAL_CAPACITY = 6,
    VN_ACTION_TERMINAL_DEADLINE = 7,
    VN_ACTION_TERMINAL_GENERATION_EXHAUSTED = 8,
    VN_ACTION_TERMINAL_MEDIA_NOT_READY = 9,
    VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE = 10,
    VN_ACTION_TERMINAL_RUNTIME_ERROR = 11,
};

typedef struct VnPostActionDesc {
    uint32_t structSize;
    uint32_t flags;
    VnActionId actionId;
    VnEmitterHandle emitter;
    uint64_t callerSeed;
    uint64_t reserved0;
} VnPostActionDesc;

typedef struct VnActionFiringInfo {
    uint32_t structSize;
    VnActionFiringState state;
    VnActionFiringOutcome outcome;
    VnActionTerminalReason terminalReason;
    VnActionId actionId;
    uint64_t worldGeneration;
    uint64_t acceptedLogicSequence;
    VnActionFiringHandle parentFiring;
    uint32_t directStepCount;
    uint32_t directStepTerminalCount;
    uint32_t childFiringCount;
    uint32_t livePlayableCount;
} VnActionFiringInfo;

typedef struct VnActionStepResult {
    uint32_t structSize;
    VnActionStepState state;
    VnResult result;
    VnActionOpcode opcode;
    VnActionId ownerActionId;
    VnObjectId targetStableId;
    uint64_t logicCommitSequence;
    VnActionFiringHandle childFiring;
    uint64_t producedPlayableInstance;
    uint32_t authoredEventIndex;
    uint32_t executionOrdinal;
    uint32_t depth;
    VnActionTargetKind targetKind;
    uint32_t capturedPlayableCount;
    uint32_t remainingPlayableCount;
} VnActionStepResult;

typedef struct VnPostActionByNameDesc {
    uint32_t structSize;
    uint32_t flags;
    const char* utf8Name;    /* exact authored name; borrowed for the call */
    uint8_t projectGuid[16]; /* all-zero = use session-bound project */
    VnEmitterHandle emitter;
    uint64_t callerSeed;
    uint64_t reserved0;
} VnPostActionByNameDesc;

typedef struct VnActionPinnedDesc {
    uint32_t structSize;
    uint32_t flags;
    uint8_t projectGuid[16];
    VnActionId actionId;
    const char* utf8ExpectedName;
    uint64_t nameBindingRevision;
    uint64_t callableFingerprint;
    VnEmitterHandle emitter;
    uint64_t callerSeed;
    uint64_t reserved0;
} VnActionPinnedDesc;

typedef struct VnActionResolved {
    uint32_t structSize;
    uint32_t reserved0;
    VnActionId actionId;
    uint64_t nameBindingRevision;
    uint64_t callableFingerprint;
    uint64_t publicationGeneration; /* PublishedView.compositeGeneration */
    uint8_t projectGuid[16];
} VnActionResolved;

/* All PostAction entry points normalize emitter=0 to the engine session's
 * persistent, private StateOnly context. Emitter-scoped selection memory,
 * variables, cooldowns and controls are shared by these default callers;
 * authored Global scope remains global. This does not attach a spatial source
 * or disable authored spatialization. A nonzero stale/private emitter is
 * VN_STALE_HANDLE, never a fallback to the default context. */
VnResult Vn_PostAction(const VnPostActionDesc* desc,
                       VnActionFiringHandle* outFiring);
VnResult Vn_PostActionByName(const VnPostActionByNameDesc* desc,
                             VnActionFiringHandle* outFiring);
VnResult Vn_ResolveActionByName(const VnPostActionByNameDesc* desc,
                                VnActionResolved* outResolved);

/* Resolves one exact variable name in the published catalog. A wrong project,
 * a retired or renamed name, and an execution fingerprint that does not match
 * the live definition use the gameplay name results. The caller writes the
 * resolved id with Vn_SetValue. Logic thread only. */
VnResult Vn_ResolveVariableByName(const VnPostActionByNameDesc* desc,
                                  VnVariableResolved* outResolved);

VnResult Vn_PostActionPinned(const VnActionPinnedDesc* desc,
                             VnActionFiringHandle* outFiring);
VnResult Vn_PostActionResolved(const VnActionResolved* resolved,
                               VnEmitterHandle emitter,
                               uint64_t callerSeed,
                               VnActionFiringHandle* outFiring);
VnResult Vn_StopActionFiring(VnActionFiringHandle firing, uint32_t fadeMs);
VnResult Vn_QueryActionFiring(VnActionFiringHandle firing,
                              VnActionFiringInfo* outInfo);
VnResult Vn_QueryActionStep(VnActionFiringHandle firing,
                            uint32_t authoredEventIndex,
                            VnActionStepResult* outResult);
VnResult Vn_ReleaseActionFiring(VnActionFiringHandle firing);
/* Accepts a source-time seek for one live root Action firing. VN_OK means the
 * complete operation was admitted, not that audio has applied it. Query the
 * returned ticket for APPLIED or CANCELLED. Refusals leave every target
 * unchanged. Relative seek is supported for resident sources only; streamed
 * seek is absolute and non-looping in V1. A firing tree with any deferred
 * Action step returns VN_CAPABILITY_UNAVAILABLE rather than partially seeking
 * only the already-materialized voices. Logic thread only. */
VnResult Vn_SeekActionFiring(const VnActionSeekDescV1* desc,
                             VnActionSeekTicket* outTicket);
VnResult Vn_QueryActionSeek(VnActionSeekTicket ticket,
                            VnActionSeekInfoV1* outInfo);
VnResult Vn_ReleaseActionSeek(VnActionSeekTicket ticket);

/* Atomically stops all logical voices in the engine. Pending-start and parked
 * voices end immediately, and live voices stop with fadeMs. The command batch is
 * preflighted before any state changes; VN_BACKPRESSURE has zero effects.
 * Registered content, emitters, buses, and authored state are preserved.
 * Logic thread only. */
VnResult Vn_StopAll(uint32_t fadeMs);

/* Bus sends: additive post-fader taps layered on the tree output. Any bus may
 * receive send edges. A connect/disconnect is a structural change — the engine
 * re-derives the bus evaluation order (so a send source always mixes before its
 * target) and applies it atomically at a block boundary; changing a send level
 * is not structural and reorders nothing. */

/* Connects a bus->bus send from `fromBus` to `toBus` at an initial level in dB.
 * The edge is additive: `toBus` receives a copy of `fromBus`'s finalized output
 * scaled by the level, on top of `toBus`'s own inputs. Refusals: an unknown bus
 * is VN_UNKNOWN_ID; fromBus == toBus (or a non-finite level) is VN_INVALID_ARG;
 * an already-connected edge is VN_IN_USE; a full per-bus send-slot table
 * (busSendSlotsPerBus) is VN_NO_SEND; an edge that would close a routing cycle
 * through the combined tree+send graph is VN_SEND_CYCLE; and a full topology
 * plan pool or command queue is VN_BACKPRESSURE (retry after Vn_Update drains
 * plan retires, or after an audio block drains, respectively). Every refusal
 * leaves the graph untouched. Logic thread only. */
VnResult Vn_ConnectBusSend(VnBusId fromBus, VnBusId toBus, float levelDb);

/* Removes a bus->bus send edge. Disconnecting an edge that does not exist is a
 * benign no-op returning VN_OK (and republishes nothing). An unknown bus is
 * VN_UNKNOWN_ID; an edge owned by a topology snapshot or Pack is VN_IN_USE; a
 * full topology plan pool or command queue is VN_BACKPRESSURE (same retry
 * conditions as Vn_ConnectBusSend), leaving the edge in place. Logic thread
 * only. */
VnResult Vn_DisconnectBusSend(VnBusId fromBus, VnBusId toBus);

/* Sets an existing bus->bus send edge's level. rampMs > 0 ramps linearly in the
 * amplitude domain from the current value, completing on a block boundary;
 * rampMs == 0 snaps at the next block boundary (the bus-volume convention). This
 * is not a structural change — it reorders nothing and publishes no topology
 * plan. Refusals: an unknown bus or an edge that is not connected is
 * VN_UNKNOWN_ID; a non-finite level is VN_INVALID_ARG; an edge owned by a
 * topology snapshot or Pack is VN_IN_USE; a full command queue is
 * VN_BACKPRESSURE with no state change. Logic thread only. */
VnResult Vn_SetBusSendLevel(VnBusId fromBus, VnBusId toBus, float levelDb,
                            uint32_t rampMs);

/* Voice sends: a voice contributes a copy of its post-pan/post-width stereo
 * block (the identical block that feeds its dry bus) to a target bus, scaled by
 * the send level — so the spatial image travels with the send. sendSlot indexes
 * the voice's send slots (0-based, below the configured voiceSendSlots); setting
 * a slot that already holds a send replaces it. Refusals: sendSlot at or beyond
 * voiceSendSlots, or a non-finite level, is VN_INVALID_ARG; an unknown target
 * bus is VN_UNKNOWN_ID; a stale handle is a benign no-op returning
 * VN_STALE_HANDLE; a full command queue is VN_BACKPRESSURE with no state change.
 * A voice awaiting a render slot stages the send and a parked voice keeps it,
 * applied when the voice (re)acquires a slot. Logic thread only. */
VnResult Vn_SetVoiceSend(VnVoiceHandle handle, uint32_t sendSlot, VnBusId toBus,
                         float levelDb);

/* Clears a voice's send slot. Clearing an empty slot is accepted. sendSlot at
 * or beyond voiceSendSlots is VN_INVALID_ARG; a stale handle is a benign no-op
 * returning VN_STALE_HANDLE; a full command queue is VN_BACKPRESSURE with no
 * state change. Logic thread only. */
VnResult Vn_ClearVoiceSend(VnVoiceHandle handle, uint32_t sendSlot);

/* Sets a voice send slot's level. Same ramp discipline as Vn_SetBusSendLevel.
 * Refusals: sendSlot at or beyond voiceSendSlots, a non-finite level, or a slot
 * that holds no active send is VN_INVALID_ARG; a stale handle is a benign no-op
 * returning VN_STALE_HANDLE; a full command queue is VN_BACKPRESSURE with no
 * state change. Logic thread only. */
VnResult Vn_SetVoiceSendLevel(VnVoiceHandle handle, uint32_t sendSlot,
                              float levelDb, uint32_t rampMs);

/* Bus insert-effect chain. Buses host the same effect types voices do, with the
 * same slot mechanics, the same preallocated pools, and the same handoff (the
 * instance is acquired and configured on the calling thread, then activated by
 * the audio thread at the next block boundary; the audio thread never
 * constructs or frees an effect). A bus is a persistent node, so unlike a
 * voice there is NO tail drain on clear or teardown — an effect's tail simply
 * flows while it is installed and stops when it is cleared. The chain runs
 * around the bus's fader point (see Vn_SetBusFaderPoint): fx slots [0,
 * faderAfterSlot) before BusVolume, the rest after. slot indexes the chain
 * (0-based, below the configured fxSlotsPerBus). */

/* Installs a built-in effect into a bus slot. `type` selects both the behavior
 * and the concrete descriptor the base pointer must point to. Setting a slot
 * that already holds an effect replaces it in place (the old instance is
 * reclaimed). Refusals: a null or wrong-size descriptor, an unknown type, an
 * out-of-range parameter, or a slot at or beyond fxSlotsPerBus are
 * VN_INVALID_ARG; an unknown bus is VN_UNKNOWN_ID; an exhausted effect pool is
 * VN_NO_EFFECT; a full command queue is VN_BACKPRESSURE with no side effects
 * (no instance is taken from the pool). Logic thread only. */
VnResult Vn_SetBusEffect(VnBusId busId, uint32_t slot, VnEffectType type,
                         const VnEffectDesc* desc);

/* Removes any effect from a bus slot at the next block boundary. The cleared
 * effect's tail is cut (buses do not drain); the slot's bypass target is kept
 * and any in-progress bypass ramp settles immediately, so the slot then behaves
 * exactly like one that never held an effect. Clearing an already-empty slot is
 * accepted. A slot at or beyond fxSlotsPerBus is VN_INVALID_ARG; an unknown bus
 * is VN_UNKNOWN_ID; a full command queue is VN_BACKPRESSURE with no state
 * change. Logic thread only. */
VnResult Vn_ClearBusEffect(VnBusId busId, uint32_t slot);

/* Updates one parameter of the effect in a bus slot. paramIndex is the effect
 * type's private 0-based index (see the tables above Vn_SetVoiceEffect); value
 * is checked against that parameter's range for the installed type. Parameters
 * carrying VN_EFFECT_PARAMETER_SUPPORTS_TRANSITION use the effect-owned 10 ms
 * transition; other parameters change at the block boundary. Refusals: a slot
 * at or beyond fxSlotsPerBus, an
 * empty slot, an unknown paramIndex, or a non-finite or out-of-range value are
 * VN_INVALID_ARG; an unknown bus is VN_UNKNOWN_ID; a full command queue is
 * VN_BACKPRESSURE with no state change. Logic thread only. */
VnResult Vn_SetBusEffectParam(VnBusId busId, uint32_t slot, uint32_t paramIndex,
                              float value);

/* Bypasses or re-enables a bus insert-effect slot. bypass01 is 0 to run the
 * slot (wet) or 1 to bypass it (dry); the change takes effect at the next block
 * boundary as a one-block linear wet/dry crossfade, so toggling never clicks.
 * The per-slot bypass is independent of an installed effect: bypassing an empty
 * slot persists, so an effect later installed into it starts bypassed. An empty
 * slot toggles nothing audible but is still accepted. bypass01 above 1, or a
 * slot at or beyond fxSlotsPerBus, is VN_INVALID_ARG; an unknown bus is
 * VN_UNKNOWN_ID; a full command queue is VN_BACKPRESSURE with no state change.
 * Logic thread only. */
VnResult Vn_SetBusEffectBypass(VnBusId busId, uint32_t slot, uint32_t bypass01);

VnResult Vn_ResetBusEffect(VnBusId busId, uint32_t effectSlot,
                           VnEffectType expectedEffectType);

VnResult Vn_SetBusEffectDynamicRoute(
    VnBusId targetBusId, uint32_t effectSlot, uint32_t routeSlot,
    const VnEffectDynamicRouteDesc* desc);

VnResult Vn_ClearBusEffectDynamicRoute(
    VnBusId targetBusId, uint32_t effectSlot, uint32_t routeSlot,
    VnEffectType expectedEffectType);

/* Repositions a bus's fader point: fx slots [0, faderAfterSlot) run pre-fader
 * (before BusVolume) and the rest post-fader. faderAfterSlot ranges over [0,
 * fxSlotsPerBus] inclusive (0 = whole chain post-fader; fxSlotsPerBus = whole
 * chain pre-fader, the default). The change applies at the next block boundary
 * as an atomic swap of the split index. Click scope: the swap is exactly
 * click-free when the gain staging commutes — every effect straddling the move
 * is linear, or BusVolume is at unity — which covers the intended use (a
 * material trim pre-fader, mix balance in BusVolume). A nonlinear effect
 * straddling the move under a non-unity BusVolume can produce a step at the
 * boundary, because moving where the gain applies genuinely changes that
 * effect's output; reposition such splits at quiet points. faderAfterSlot above
 * fxSlotsPerBus is VN_INVALID_ARG; an unknown bus is VN_UNKNOWN_ID; a full
 * command queue is VN_BACKPRESSURE with no state change. Logic thread only. */
VnResult Vn_SetBusFaderPoint(VnBusId busId, uint32_t faderAfterSlot);

/* Hot reload: the replace verbs. Each swaps a registered asset's content in a
 * running engine under the retire-old / new-plays-new migration contract. The
 * id (or handle) must name a CURRENTLY registered asset of that same class —
 * VN_UNKNOWN_ID (or VN_STALE_HANDLE for the pack handle) otherwise; replace
 * never implicitly registers, and register/unregister keep their existing
 * contracts. The new descriptor is validated fully and up front, identically
 * to the corresponding register path, and ANY refusal leaves the old content
 * fully in force (all-or-nothing per call).
 *
 * Media migration (PCM, stream, pack): every logical voice that exists when
 * the replace returns — live, awaiting a deferred start, or parked — keeps
 * consuming the OLD content generation to its natural end; the old
 * generation's memory (or open file) stays pinned until its last such voice
 * drains, then retires on the logic side. Plays issued after the replace
 * resolve the NEW content. No voice migrates mid-flight and nothing
 * crossfades — a playing sound never changes its data. A parked voice that
 * returns resumes on the generation it started on. At most ONE retiring
 * generation exists per asset: a second replace while the previous old
 * generation still has logical voices is refused VN_IN_USE (retry after those
 * voices drain, or stop them). Snapshot counters hotReloadCount,
 * hotReloadRefusedCount, pinnedGenerationCount, and pinnedGenerationHighWater
 * observe all of this, and every replace outcome writes a log-ring record
 * naming the asset class and id. Logic thread only, like the registration
 * surfaces they mirror. */

/* Replaces a registered in-memory sound's PCM. Validation is Vn_RegisterPCM's
 * (VN_INVALID_ARG on any descriptor fault); VN_UNKNOWN_ID when the id does not
 * name registered PCM (a stream, container, or unknown id never matches);
 * VN_IN_USE while a previous replace of this id is still draining. Under
 * VN_PCM_POLICY_REFERENCE the ownership rule follows the generation: the OLD
 * registration's memory must stay pinned until its generation retires (the
 * same window in which unregister would have been refused VN_IN_USE), and the
 * NEW memory from this call until its own unregister or replace. */
VnResult Vn_ReplacePCM(VnSoundId soundId, const VnPCMDesc* desc);

/* Replaces a registered streamed sound's backing .stream file. The new file
 * is opened and validated end to end by the streaming thread during this call
 * (the Vn_RegisterStreamFile contract, including the TOC-carries-this-id
 * check); any failure is VN_BAD_STREAM_FILE with the old registration
 * untouched. VN_UNKNOWN_ID when the id names no registered stream; VN_IN_USE
 * while a previous replace of this id is still draining; VN_DEVICE_ERROR when
 * the streaming thread has stopped (see Vn_RegisterStreamFile). Voices already
 * playing (or pending) keep streaming from the old file, which closes when
 * the last of them drains. */
VnResult Vn_ReplaceStreamFile(VnSoundId soundId, const char* utf8Path);

/* Replaces a loaded pack wholesale with the container at desc->utf8Path. The
 * new file loads and validates fully first (any failure is VN_BAD_PACK — or
 * VN_IN_USE when a new entry id is owned by a registration outside this pack
 * — with the old pack untouched); the entries then swap as ONE generation.
 * The registry reflects the new pack's TOC the moment this call returns: an
 * id absent from the new TOC fails VN_UNKNOWN_ID for every new resolution or
 * play immediately, and the retiring old generation is reachable only through
 * logical voices created before the replace. The pack handle survives; a
 * stale handle is a benign no-op returning VN_STALE_HANDLE. VN_IN_USE also
 * refuses a replace while this pack's previous old generation still has
 * logical voices. */
VnResult Vn_ReplacePack(VnPackHandle handle, const VnPackDesc* desc);

/* Replaces a registered attenuation profile's content. Validation is
 * Vn_RegisterAttenuation's; VN_UNKNOWN_ID when the id names no registered
 * profile. Logic-side curves need no generation: voices bound to the profile
 * re-evaluate against the new curves on the next Vn_Update through the
 * parameter system's normal dirty path. */
VnResult Vn_ReplaceAttenuation(VnAttenuationId id,
                               const VnAttenuationDesc* desc);

/* Session trace: a journal of the public API call stream — every call with
 * its arguments (descriptors deep-copied at their declared vintage size),
 * the engine config recorded at Vn_Init (including randomSeed), and the
 * tick/block schedule as explicit barrier records (Vn_Update begin/end and
 * every rendered block), each record carrying a monotonic ordinal. Replay
 * (`harken_host trace-replay`) re-executes the journal on the null backend in
 * the recorded interleaving; the engine is deterministic given (config,
 * seed, API sequence, tick schedule, content files), so the same journal
 * renders byte-identically on every replay. Wall-clock timestamps in the
 * journal are analysis metadata only; replay ignores them. Content files
 * (packs, streams) are referenced by path plus content hash, never
 * embedded — a hash mismatch at replay refuses the replay with a report
 * naming the file. In-memory arguments (PCM sample data, Step lists,
 * container children) are argument data, not content files, and are copied
 * into the journal.
 *
 * The trace layer is a logic-thread API journal: it adds nothing to the
 * render contract and never touches the audio thread. Tracing OFF costs one
 * branch per API call on the logic thread and nothing on the audio thread.
 * File I/O runs buffered on the logic thread (the pack-load precedent). */

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

/* Multichannel output: per-bus output layout and downmix matrix. Beyond
 * stereo, the engine renders every bus at the mix layout (VnConfig.
 * mixChannelLayout) and folds to a narrower layout at two boundaries — the
 * master->device output (when the device is narrower than the mix) and any bus
 * configured with a narrower output layout. Each fold applies THAT bus's
 * downmix matrix (the ITU-R BS.775 default until overridden). At the stereo
 * default the whole amendment is inert. */

/* Channel layouts (VnConfig.mixChannelLayout / .deviceChannelLayout,
 * VnDownmixMatrixDesc.srcLayout / .dstLayout, Vn_SetBusOutputLayout). Values
 * are ABI-fixed and never renumbered; the canonical channel order of each
 * concrete layout is fixed (SMPTE / WAVE_FORMAT_EXTENSIBLE order, matching the
 * device so the runtime and device agree without remapping):
 *   MONO   : C
 *   STEREO : FL, FR
 *   QUAD   : FL, FR, BL, BR
 *   5.1    : FL, FR, FC, LFE, BL, BR
 *   7.1    : FL, FR, FC, LFE, BL, BR, SL, SR
 * VN_CHANNEL_LAYOUT_DEFAULT (0) is a sentinel meaning "this field's documented
 * default", never a concrete layout: it is not a valid srcLayout/dstLayout for
 * a downmix matrix, nor a valid Vn_SetBusOutputLayout argument. Defined here in
 * the tail-append region (the multichannel amendment is a pure append); the
 * uint32_t layout fields of VnConfig / VnSnapshot need no forward declaration. */
typedef uint32_t VnChannelLayout;
enum {
    VN_CHANNEL_LAYOUT_DEFAULT = 0, /* sentinel: this field's documented default */
    VN_CHANNEL_LAYOUT_MONO = 1,
    VN_CHANNEL_LAYOUT_STEREO = 2,
    VN_CHANNEL_LAYOUT_QUAD = 3,
    VN_CHANNEL_LAYOUT_5_1 = 4,
    VN_CHANNEL_LAYOUT_7_1 = 5
};

/* A dst x src downmix coefficient table. Packing is authoritative:
 * coeffs[r*8 + c] is the gain from source channel c into destination channel r
 * (row-major, destination the outer index). Only the sub-block
 * [0, dstChannels) x [0, srcChannels) is read; the rest of the fixed 8x8 array
 * is ignored. srcLayout / dstLayout are concrete VnChannelLayout values (never
 * VN_CHANNEL_LAYOUT_DEFAULT) with dstLayout narrower-or-equal to srcLayout. The
 * engine applies coefficients as given and never normalizes for overload —
 * headroom is an authoring choice baked into the coefficients; the device
 * boundary is the honest clip point. */
typedef struct VnDownmixMatrixDesc {
    uint32_t structSize; /* set to sizeof(VnDownmixMatrixDesc) */
    uint32_t srcLayout;  /* VnChannelLayout the fold reads (never DEFAULT) */
    uint32_t dstLayout;  /* VnChannelLayout the fold writes (narrower-or-equal) */
    uint32_t reserved0;  /* keeps 8-byte alignment for the coefficient array */
    float coeffs[8 * 8]; /* coeffs[r*8 + c] = gain src c -> dst r */
} VnDownmixMatrixDesc;

/* Configures a bus's output layout: the bus renders its chain internally at the
 * mix layout, then folds to `layout` (a concrete VnChannelLayout
 * narrower-or-equal to the mix) at its output boundary through its own downmix
 * matrix, before its output is scattered to its parent and sends. `layout`
 * equal to the mix layout (the default, also selectable) means no fold — prior
 * behavior. This is the per-bus, non-master fold point. Refusals: a layout
 * wider than the mix, or VN_CHANNEL_LAYOUT_DEFAULT, is VN_INVALID_ARG; a layout
 * the build cannot honor is VN_UNSUPPORTED_LAYOUT; an unknown bus is
 * VN_UNKNOWN_ID. Setting the output layout is a structural change published at
 * the next block boundary, so a momentarily unavailable resource (a full
 * topology-plan pool or command queue) is VN_BACKPRESSURE with no state change.
 * Logic thread only. */
VnResult Vn_SetBusOutputLayout(VnBusId busId, uint32_t layout);

/* Sets the coefficient matrix a bus uses for its fold. The desc's srcLayout /
 * dstLayout describe the fold this bus performs: for a non-master bus,
 * src = the mix layout and dst = the bus's output layout; for the master,
 * dst = the device layout. Refusals: a null or wrong-size desc, non-finite
 * coefficients, or a src/dst pair that is not narrower-or-equal are
 * VN_INVALID_ARG; a layout pair the build cannot honor is
 * VN_UNSUPPORTED_LAYOUT; an unknown bus is VN_UNKNOWN_ID. Like
 * Vn_SetBusOutputLayout the matrix is published at the next block boundary, so
 * a full topology-plan pool or command queue is VN_BACKPRESSURE with no state
 * change. Logic thread only. */
VnResult Vn_SetBusDownmixMatrix(VnBusId busId, const VnDownmixMatrixDesc* desc);

/* Reverts a bus to its default downmix matrix: ITU-R BS.775 where BS.775
 * defines the fold (the 5.1 source folds), the documented engineering
 * extension for the 7.1 folds. An unknown bus is VN_UNKNOWN_ID; a full
 * topology-plan pool or command queue is VN_BACKPRESSURE with no state change.
 * Logic thread only. */
VnResult Vn_ResetBusDownmixMatrix(VnBusId busId);

/* Public binding system: registered Binding definitions, explicit
 * attachments, and per-voice instances — one Variable, through one curve,
 * to one scoped target, with a transition time. Registration has NO
 * audible side effect; activation is always an explicit attachment (or a
 * per-voice apply). All pools are fixed at Vn_Init and preallocated; the
 * audio thread never learns bindings exist — every resolved change rides
 * the existing parameter commands. Logic thread only throughout. */

typedef VnObjectId VnBindingId;

/* Built-in VOICE-scope target ids. Values are ABI-fixed and append-only. */
enum {
    VN_TARGET_VOICE_VOLUME = 0,
    VN_TARGET_VOICE_PITCH = 1,
    VN_TARGET_VOICE_LOWPASS_CUTOFF = 2,
    VN_TARGET_VOICE_HIGHPASS_CUTOFF = 3,
    VN_TARGET_VOICE_PAN = 4,
    VN_TARGET_VOICE_WIDTH = 5,
    VN_TARGET_VOICE_LOW_SHELF_GAIN_DB = 6,
    VN_TARGET_VOICE_LOWPASS_Q = 7,
    VN_TARGET_VOICE_HIGHPASS_Q = 8,
    VN_TARGET_VOICE_LOW_SHELF_FREQUENCY_HZ = 9,
    VN_TARGET_VOICE_LOW_SHELF_Q = 10,
    VN_TARGET_VOICE_HIGH_SHELF_GAIN_DB = 11,
    VN_TARGET_VOICE_HIGH_SHELF_FREQUENCY_HZ = 12,
    VN_TARGET_VOICE_HIGH_SHELF_Q = 13
};

/* Scope kinds of the target locator (scopeKind, objectId, targetId).
 * Values are ABI-fixed and append-only; a scope kind this engine does not
 * know is refused VN_INVALID_ARG, never guessed. */
enum {
    VN_TARGET_SCOPE_VOICE = 0, /* the fourteen VN_TARGET_VOICE_* targets;
                                * the object is the voice a binding instance
                                * lives on */
    VN_TARGET_SCOPE_BUS = 1,   /* bus targets; targetId 0 = BusVolume (dB,
                                * add-in-dB, neutral 0); targetId 1 =
                                * BusOutputVolume (dB, add-in-dB, neutral 0,
                                * outputOnly); the object is a VnBusId
                                * (VN_BUS_MASTER included) */
    VN_TARGET_SCOPE_VOICE_EFFECT_PARAM = 2, /* a parameter of an insert effect
                                * in one of a voice's fx slots. targetId is the
                                * packed locator: bits 0..5 the effect type's
                                * private parameter index (0..63), bits 6..8 the
                                * fx slot (0..7), bits 9..31 the expected effect
                                * type (the installed effect must match). The
                                * object is the live voice the binding is applied
                                * to. Contribution domain, base, and clamp come
                                * from the generated effect-parameter metadata
                                * authority, per (effect type, parameter index),
                                * not this scope. */
    VN_TARGET_SCOPE_BUS_EFFECT_PARAM = 3 /* a parameter of an insert effect in
                                * one of a bus's fx slots. targetId packs the
                                * same locator as VN_TARGET_SCOPE_VOICE_EFFECT_-
                                * PARAM; the object is a VnBusId. */
};

/* Control-point capacity of one binding curve. */
#define VN_CURVE_MAX_POINTS 8

/* One binding curve control point: x is the Variable's value, y is the
 * target's contribution in its own domain (dB for gain targets, cents
 * for pitch, octave offsets for AddLogHz/AddLogQ targets). */
typedef struct VnCurvePoint {
    float x, y;
} VnCurvePoint;

/* One binding curve: piecewise cubic Bezier segments over strictly
 * increasing x. Handles use absolute coordinates and each handle x lies
 * within its segment interval; h1x <= h2x is not required. All point and
 * handle coordinates must be finite; binding y values have no additional
 * generic range beyond finiteness. Unused handle slots (i >= pointCount - 1)
 * must be bitwise +0.0f. Outside the x span the curve clamps
 * to its end values. VnCurveDesc mirrors VnAttenuationCurve so the same
 * curve kernel evaluates both. */
typedef struct VnCurveDesc {
    uint32_t structSize;                      /* set to sizeof(VnCurveDesc) */
    uint32_t pointCount;                      /* 1..VN_CURVE_MAX_POINTS(8) */
    VnCurvePoint points[8];                   /* strictly increasing x */
    VnCurveHandlePair handles[7];             /* one per active segment */
} VnCurveDesc;

/* One Binding definition: the immutable-until-replaced mapping row. The
 * definition carries no object identity — attachments (below) activate it
 * against concrete objects, so one definition can fan out to many. */
typedef struct VnBindingDesc {
    uint32_t structSize;                      /* set to sizeof(VnBindingDesc) */
    VnVariableId variableId;                  /* input */
    uint32_t targetScopeKind;                 /* VN_TARGET_SCOPE_VOICE / _BUS */
    uint32_t targetId;                        /* scoped target table, above */
    VnCurveDesc curve;
    uint32_t transitionMs;                    /* 0..60000 ms; ramp requested when this
                                               * binding's contribution
                                               * changes; 0 = snap. Nonzero is
                                               * legal only on targets whose
                                               * wire command carries a ramp —
                                               * volume, pitch, and fixed Tone
                                               * targets — and is refused
                                               * VN_INVALID_ARG loudly on every
                                               * other target, never silently
                                               * ignored. */
    uint32_t reserved0;                       /* explicit tail padding */
} VnBindingDesc;

/* One attachment relation: activates a registered definition against one
 * object. Attachments carry no curves; definitions carry no objects. */
typedef struct VnBindingAttachDesc {
    uint32_t structSize;     /* set to sizeof(VnBindingAttachDesc) */
    uint32_t scopeKind;      /* must equal the definition's targetScopeKind */
    VnObjectId objectId;       /* current-world content host or registered Sound GUID (VOICE), VnBusId (BUS) */
} VnBindingAttachDesc;

/* Registers a Binding definition under a caller-minted id. The descriptor
 * is validated exhaustively before any pool state changes — curve
 * finiteness and strictly increasing x, one valid handle pair per segment,
 * known scoped target, transitionMs legal for the target — and a refused call
 * has zero side effects. Registration alone changes no sound: nothing
 * evaluates until an attachment (or Vn_ApplyVoiceBinding) activates the
 * definition. Refusals: a null or wrong-size descriptor (the embedded curve's
 * structSize included) or any invalid field is VN_INVALID_ARG; an id that
 * already names a binding is VN_IN_USE; a full definition pool is
 * VN_NO_BINDING with a log record naming the pool. Logic thread only. */
VnResult Vn_RegisterBinding(VnBindingId bindingId, const VnBindingDesc* desc);

/* Removes a Binding definition. Refused VN_IN_USE while ANY reference to
 * it exists — an attachment, a live voice instance, or a parked or
 * pending-start voice holding the instance identity. Detach first; there
 * is no hidden cascade. VN_UNKNOWN_ID when the id names no binding. Logic
 * thread only. */
VnResult Vn_UnregisterBinding(VnBindingId bindingId);

/* Replaces a registered definition's content live, in place. Structural
 * identity is frozen: variableId, targetScopeKind, and targetId must equal
 * the registered definition's, else VN_INVALID_ARG — only the curve and
 * transitionMs may change (re-pointing a binding is detach + unregister +
 * register, by design). The replacement validates fully (the registration
 * rules), then installs as a new definition revision at the next
 * Vn_Update; from that tick on every reader — bus attachments, live voice
 * instances, parked and pending-start instances — evaluates the new
 * revision, and changed resolved values publish with the NEW definition's
 * transitionMs. Same-tick multiple replaces install last-wins, each call's
 * result reported individually. Any refusal leaves the old content fully
 * in force. VN_UNKNOWN_ID when the id names no binding. Logic thread
 * only. */
VnResult Vn_ReplaceBinding(VnBindingId bindingId, const VnBindingDesc* desc);

/* Activates a definition against one object. BUS attachments take effect
 * at the next Vn_Update — the definition's transitionMs governs the entry
 * ramp — and resolve their Variable at GLOBAL scope only (a bus has no
 * emitter context). VOICE attachments activate future plays: a play whose
 * Sound id carries attachments instantiates them on the new voice. One
 * definition may hold many attachments; the pool is global and fixed.
 * Refusals: a null or wrong-size descriptor, or a scopeKind that does not
 * equal the definition's targetScopeKind, is VN_INVALID_ARG; an unknown
 * binding id, bus, or playable is VN_UNKNOWN_ID; a duplicate
 * (definition, object) attachment is VN_IN_USE; a full attachment pool is
 * VN_NO_BINDING with a log record naming the pool; an attachment whose
 * worst-case publication demand would exceed the command queue's capacity
 * is VN_INVALID_CONFIG (structural — clear by detaching or configuring a
 * larger queue; the log record carries both numbers). Logic thread
 * only. */
VnResult Vn_AttachBinding(VnBindingId bindingId,
                          const VnBindingAttachDesc* desc);

/* Deactivates one attachment. A BUS detach ramps the contribution back to
 * neutral over the definition's transitionMs. A VOICE detach stops future
 * instantiation AND neutralizes the live instances born from this
 * attachment (contribution ramps to neutral with the definition's
 * transitionMs; the instance slots free when their voices end) — but a
 * pending-start voice keeps its pinned attachment set: an already-resolved
 * queued play is never altered after acceptance. Refusals: a null or
 * wrong-size descriptor is VN_INVALID_ARG; an attachment that does not
 * exist is VN_UNKNOWN_ID. Logic thread only. */
VnResult Vn_DetachBinding(VnBindingId bindingId,
                          const VnBindingAttachDesc* desc);

/* Creates one binding instance on one live voice — the per-play special
 * case of attachment, same validation, same instance pool. The instance
 * evaluates from the next Vn_Update; its first resolved values snap (a
 * ramp from neutral would be a wrong-direction sweep). Refusals: an
 * unknown binding id is VN_UNKNOWN_ID; a definition whose scope is not
 * VN_TARGET_SCOPE_VOICE is VN_INVALID_ARG; a stale or ended handle is a
 * benign no-op returning VN_STALE_HANDLE; an instance of this definition
 * already live on the voice is VN_IN_USE; a full per-voice instance table
 * (voiceBindingInstanceSlots) is VN_NO_BINDING with a log record naming
 * the pool; an instance whose worst-case publication demand would exceed
 * the command queue's capacity is VN_INVALID_CONFIG (the attachment
 * contract's capacity rule). Logic thread only. */
VnResult Vn_ApplyVoiceBinding(VnVoiceHandle handle, VnBindingId bindingId);

/* Neutralizes one binding instance on one live voice: the contribution
 * ramps to neutral with the definition's transitionMs and the slot frees
 * when the voice ends. A stale or ended handle is a benign no-op returning
 * VN_STALE_HANDLE; a voice holding no instance of this definition is
 * VN_UNKNOWN_ID. Logic thread only. */
VnResult Vn_ClearVoiceBinding(VnVoiceHandle handle, VnBindingId bindingId);

enum VnBusMeterTap {
    VN_BUS_METER_TAP_POST_EFFECTS = 0,
    VN_BUS_METER_TAP_PRE_FADER = 1,
    VN_BUS_METER_TAP_POST_FADER = 2
};

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
 * Session file/wire integers are little-endian and must not be a native
 * memcpy of these C structs. File magic/commit magic are 8-byte ASCII
 * NUL-terminated markers encoded as little-endian uint64 constants
 * (VNMON10 / VNCMT10).
 * VN_MONITOR_SESSION_CHUNK_HEADER_BYTES / COMMIT_FOOTER_BYTES are the
 * packed on-disk sizes from PLAN-10 (32 / 24), not sizeof the in-memory
 * VnMonitorSessionChunkHeader / VnMonitorSessionCommitFooter.
 * deliverySequence in VnMonitorRecordHeader is consumer-local: Session
 * files and Remote transcripts store 0; only Vn_MonitorPoll fills it.
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
typedef uint32_t VnMonitorComparisonResult;
typedef uint32_t VnMonitorPlanActivationState;
typedef uint32_t VnMonitorStateCutResult;
typedef uint32_t VnMonitorMeasurementKind;
typedef uint32_t VnMonitorValidityFlags;
typedef uint32_t VnMonitorRemoteOpcode;
typedef uint32_t VnMonitorRecordingState;
typedef uint32_t VnMonitorChannelMode;
typedef uint32_t VnMonitorChunkKind;
typedef uint32_t VnMonitorChunkFlags;

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
    VN_MONITOR_DROP_RECORDER_QUEUE = 3,
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
    VN_MONITOR_PERM_RECORD = 1u << 4,
    VN_MONITOR_PERM_RAW_AUDIO = 1u << 5,
    VN_MONITOR_PERM_SYMBOLS = 1u << 6
};

enum {
    VN_MONITOR_SECURITY_DISABLED = 0,
    VN_MONITOR_SECURITY_LOCAL_ACL = 1,
    VN_MONITOR_SECURITY_LAN_MTLS = 2
};

enum {
    VN_MONITOR_COMPARE_COMPARABLE = 0,
    VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE = 1,
    VN_MONITOR_COMPARE_INCOMPARABLE = 2,
    VN_MONITOR_COMPARE_MISSING_DATA = 3
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
    VN_MONITOR_MEASURE_LUFS_SHORT_TERM = 10,
    VN_MONITOR_MEASURE_LUFS_INTEGRATED = 11
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
    VN_MONITOR_REMOTE_OP_PLAN = 5,
    VN_MONITOR_REMOTE_OP_RECORDING = 6
};

enum {
    VN_MONITOR_RECORDING_STATE_STOPPED = 0,
    VN_MONITOR_RECORDING_STATE_ACTIVE = 1,
    VN_MONITOR_RECORDING_STATE_STOPPING = 2,
    VN_MONITOR_RECORDING_STATE_FAILED = 3
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

enum {
    VN_MONITOR_CHUNK_REQUIRED = 1u << 0,
    VN_MONITOR_CHUNK_OPTIONAL = 1u << 1
};

enum {
    VN_MONITOR_CHUNK_SESSION_HEADER = 1,
    VN_MONITOR_CHUNK_CAPABILITIES = 2,
    VN_MONITOR_CHUNK_FINGERPRINTS = 3,
    VN_MONITOR_CHUNK_CLOCK_ANCHORS = 4,
    VN_MONITOR_CHUNK_OPTIONAL_SYMBOLS = 5,
    VN_MONITOR_CHUNK_STATE = 6,
    VN_MONITOR_CHUNK_EVENTS = 7,
    VN_MONITOR_CHUNK_TIME_SERIES = 8,
    VN_MONITOR_CHUNK_OPTIONAL_AUDIO = 9,
    VN_MONITOR_CHUNK_TRACE_CORRELATION = 10,
    VN_MONITOR_CHUNK_DROP_SUMMARIES = 11,
    VN_MONITOR_CHUNK_FINAL_INDEX = 12,
    VN_MONITOR_CHUNK_RECOVERY_INDEX = 13,
    VN_MONITOR_CHUNK_EXECUTION_DESCRIPTOR = 14,
    VN_MONITOR_CHUNK_EXECUTION_RECORD = 15,
    VN_MONITOR_CHUNK_EXECUTION_END = 16
};

#define VN_MONITOR_SESSION_FORMAT_MAJOR 1u
#define VN_MONITOR_SESSION_FORMAT_MINOR 0u
#define VN_MONITOR_SESSION_FILE_MAGIC 0x30314e4f4d4e56ull
#define VN_MONITOR_SESSION_COMMIT_MAGIC 0x3031544d434e56ull
#define VN_MONITOR_SESSION_CHUNK_HEADER_BYTES 32u
#define VN_MONITOR_SESSION_COMMIT_FOOTER_BYTES 24u
#define VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES 316u
#define VN_MONITOR_STATE_ENTITY_SLOTS 16u
#define VN_MONITOR_DEFAULT_SESSION_FILE_BYTES 8589934592ull
#define VN_MONITOR_HARD_SESSION_FILE_BYTES 68719476736ull
#define VN_MONITOR_DEFAULT_CHUNK_PAYLOAD_BYTES 67108864ull
#define VN_MONITOR_HARD_CHUNK_PAYLOAD_BYTES 268435456ull
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
#define VN_MONITOR_DEFAULT_RECORDER_QUEUE_BYTES 67108864ull
#define VN_MONITOR_HARD_RECORDER_QUEUE_BYTES 536870912ull
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
    uint64_t resolvedRecorderQueueBytes;
    uint64_t hardRecorderQueueBytes;
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
    VN_MONITOR_FACT_PLAYABLE_FINISHED = 43
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

typedef struct VnMonitorRecordingDesc {
    uint32_t structSize;
    uint32_t reserved0;
    const char* utf8Path;
    uint64_t fileByteBudget;
} VnMonitorRecordingDesc;

typedef struct VnMonitorRecordingStatus {
    uint32_t structSize;
    uint32_t state;
    uint64_t fileBytes;
    uint64_t lastCommittedChunk;
    uint64_t queueBytes;
    uint32_t recoveryNeeded;
    uint32_t errorReason;
} VnMonitorRecordingStatus;

/* Offline compare input. Callers recover first (M4-2); this struct is a
 * projected view, not a file path and not SessionRecoverResult. */
typedef struct VnMonitorSessionCompareView {
    uint32_t structSize;
    uint32_t status;
    uint32_t recoveryNeeded;
    uint32_t droppedRequired;
    uint32_t requiredUnknown;
    uint32_t crcFailedRequired;
    uint32_t truncatedAfterCommit;
    uint32_t hasCompleteFooter;
    uint32_t hasTraceCorrelation;
    uint32_t hasOptionalSymbols;
    uint64_t lastCompleteChunkSequence;
    uint64_t fileBytes;
    uint64_t bytesConsumed;
    uint64_t capabilityFingerprint;
    uint64_t programSemanticFingerprint;
    uint64_t runtimeLineageFingerprint;
    uint64_t tapDescriptorId;
    uint64_t measurementSegmentGeneration;
    uint32_t sampleRate;
    uint32_t algorithmId;
    uint32_t algorithmVersion;
    uint32_t unitId;
    uint32_t targetStreamKindMask;
    uint32_t presentStreamKindMask;
    uint64_t rangeBegin;
    uint64_t rangeEnd;
    uint32_t gapInvalidWarmup;
    uint32_t reserved0;
} VnMonitorSessionCompareView;

typedef struct VnMonitorSessionCompareDesc {
    uint32_t structSize;
    uint32_t reserved0;
    const VnMonitorSessionCompareView* left;
    const VnMonitorSessionCompareView* right;
    uint64_t executeFrameBegin;
    uint64_t executeFrameEnd;
} VnMonitorSessionCompareDesc;

typedef struct VnMonitorCompareResult {
    uint32_t structSize;
    uint32_t result;
} VnMonitorCompareResult;

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


typedef struct VnMonitorSessionFileHeader {
    uint32_t structSize;
    uint32_t formatMajor;
    uint32_t formatMinor;
    uint32_t reserved0;
    uint64_t fileMagic;
    uint64_t headerBytes;
    uint64_t engineBuildId;
    uint64_t engineMonitorInstanceId;
    uint64_t createdUtcTicks;
    uint64_t capabilityFingerprint;
    uint32_t headerCrc32c;
    uint32_t reserved1;
} VnMonitorSessionFileHeader;

/* In-memory description of one on-disk chunk. Packed file bytes are:
 * kind:u16, flags:u16, version:u16, headerBytes:u16, payloadBytes:u64,
 * chunkSequence:u64, payloadCrc32c:u32, headerCrc32c:u32 (32 bytes LE).
 * Do not memcpy this struct to disk. */
typedef struct VnMonitorSessionChunkHeader {
    uint32_t structSize;
    uint32_t kind;
    uint32_t flags;
    uint32_t version;
    uint32_t headerBytes;
    uint32_t payloadCrc32c;
    uint32_t headerCrc32c;
    uint32_t reserved0;
    uint64_t payloadBytes;
    uint64_t chunkSequence;
} VnMonitorSessionChunkHeader;

/* In-memory description of one on-disk commit footer. Packed file bytes are:
 * commitMagic:u64 (VNCMT10), chunkSequence:u64, payloadBytes:u64 (24 bytes LE).
 * Do not memcpy this struct to disk. */
typedef struct VnMonitorSessionCommitFooter {
    uint32_t structSize;
    uint32_t reserved0;
    uint64_t commitMagic;
    uint64_t chunkSequence;
    uint64_t payloadBytes;
} VnMonitorSessionCommitFooter;

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

/* Optional SDK diagnostics. No registration transfers emitter ownership or
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

VnResult Vn_RemoteStart(const VnRemoteConfig* config);
VnResult Vn_RemoteStop(void);
VnResult Vn_RemoteGetStatus(VnRemoteStatus* outStatus);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HARKEN_HARKEN_H */
