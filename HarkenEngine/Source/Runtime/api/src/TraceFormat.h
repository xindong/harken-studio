// VTRC — the session-trace container format, shared by the logic-thread
// recorder (trace_recorder) and the host replayer (harken_host trace-replay).
// Versioned binary, little-endian, chunked: every record is length-prefixed
// and self-delimiting, so a truncated journal (a crash capture) parses up to
// its last complete record.
//
// File layout:
//   FileHeader (fixed fields below), then the versioned architecture-neutral
//   config schema, then records back to back until EOF or a TraceEnd record.
//
// Ordering contract: every record carries a monotonic ordinal. Barrier
// records mark Vn_Update begin/end (update index) and each rendered block
// (block index); API-call records sit at their ordinal position between
// barriers. Replay re-executes calls in the recorded interleaving against
// the recorded block/tick schedule. Wall-clock timestamps are analysis
// metadata only; replay ignores them.
//
// The block barriers are the logic thread's observations of the published
// audio block counter (the snapshot double buffer), sampled at every record
// boundary. On the null backend blocks advance only between logic-thread
// records, so the observation schedule is exact; a trace recorded against a
// real device attributes blocks rendered concurrently with a call to the
// next observation point, which is the only schedule the logic thread (and
// therefore the engine's logic state) ever saw.
#ifndef HARKEN_API_TRACE_FORMAT_H
#define HARKEN_API_TRACE_FORMAT_H

#include <cstdint>

namespace harken {

inline constexpr char kTraceMagic[4] = {'V', 'T', 'R', 'C'};
// Current writer vintage. The replayer also retains the historical v2 decoder.
inline constexpr std::uint32_t kTraceFormatVersionV2 = 2;
inline constexpr std::uint32_t kTraceFormatVersionV3 = 3;
inline constexpr std::uint32_t kTraceFormatVersionV4 = 4;
inline constexpr std::uint32_t kTraceFormatVersion = 6;

// Fixed-size header fields ahead of the config image. Serialized field by
// field (never as a struct overlay — the container-format discipline).
//   char     magic[4]
//   u32      formatVersion
//   u32      headerBytes          offset of the first record from file start
//   u32      configVocabulary     v2: native VnConfig structSize;
//                                 v3+: stable semantic config vintage
//   u64      baseBlocksRendered   published block count at Vn_TraceStart;
//                                 replay pumps these (provably silent) blocks
//                                 before the first record so absolute block
//                                 arithmetic (delay due-frames) reproduces
//   u64      cpuTicksPerSecond    recorder QPC rate; analysis aid only
inline constexpr std::uint32_t kTraceFixedHeaderBytes = 4 + 4 + 4 + 4 + 8 + 8;
inline constexpr std::uint32_t kTraceConfigSchemaVersion = 1;
inline constexpr std::uint32_t kTraceConfigVintageCount = 16;
// Vintage 14 appends one 32-bit firing-capacity field to the unchanged
// vintage-13 image. Vintage 15 appends the Pack catalog/resident budgets.
// Vintage 16 appends the Action cooldown capacity.
// Reserved native fields are never serialized.
inline constexpr std::uint32_t kTraceConfigWireBytesV13 =
    4 + 4 + 2 * 4 + 49 * 4 + 8;
inline constexpr std::uint32_t kTraceConfigWireBytesV14 =
    kTraceConfigWireBytesV13 + 4;
inline constexpr std::uint32_t kTraceConfigWireBytesV15 =
    kTraceConfigWireBytesV14 + 4 + 6 * 8;
inline constexpr std::uint32_t kTraceConfigWireBytes =
    kTraceConfigWireBytesV15 + 4;
// Repository history emitted VTRC v2 before and after the append-only
// Bus-topology VnConfig tail. These are the only full config-image sizes a v2
// reader may accept. A future VnConfig append requires VTRC v3.
inline constexpr std::uint32_t kTraceConfigImageLegacyBytes = 264;
inline constexpr std::uint32_t kTraceConfigImageCurrentBytes = 280;

// Record header, ahead of every record payload:
//   u32 payloadBytes   bytes after this header
//   u16 recordType     kTraceRecord*
//   u16 flags          kTraceFlag*
//   u64 ordinal        monotonic across all record types
//   u64 timestampUs    microseconds since Vn_TraceStart; analysis only
inline constexpr std::uint32_t kTraceRecordHeaderBytes = 4 + 2 + 2 + 8 + 8;

// Record types.
inline constexpr std::uint16_t kTraceRecordApiCall = 1;     // u16 callId,
                                                            // u16 reserved,
                                                            // args
inline constexpr std::uint16_t kTraceRecordBlockBarrier = 2; // u64 blockIndex
inline constexpr std::uint16_t kTraceRecordUpdateBegin = 3;  // u64 updateIndex
inline constexpr std::uint16_t kTraceRecordUpdateEnd = 4;    // u64 updateIndex
inline constexpr std::uint16_t kTraceRecordEnd = 5;          // empty payload
inline constexpr std::uint16_t kTraceRecordCurrentWorldInstall = 6;
inline constexpr std::uint16_t kTraceRecordMeterProbeInstall = 7;
inline constexpr std::uint16_t kTraceRecordMeterProbeUninstall = 8;
inline constexpr std::uint16_t kTraceRecordMeterProbeBindingDefine = 9;
inline constexpr std::uint16_t kTraceRecordMeterProbeBindingUndefine = 10;
inline constexpr std::uint16_t kTraceRecordMeterProbeBindingAttach = 11;
inline constexpr std::uint16_t kTraceRecordMeterProbeBindingDetach = 12;
inline constexpr std::uint16_t kTraceRecordVariableOutcome = 13;
inline constexpr std::uint16_t kTraceRecordProjectGenerationInstallAck = 14;
inline constexpr std::uint16_t kTraceRecordPreparedTopologyPlan = 15;
inline constexpr std::uint32_t kTracePreparedTopologyPlanVintage = 1;
// Token U64×4 + publishedBusFingerprint + compileTargetProfileFingerprint +
// U32 (publicationVintage or admission). The arithmetic is 52, not 60.
inline constexpr std::uint32_t kTraceCurrentWorldInstallPairedTailBytes =
    (4u * 8u) + 8u + 8u + 4u;
inline constexpr std::uint32_t kTracePreviewPlayPairedTailBytes =
    (4u * 8u) + 8u + 8u + 4u;
inline constexpr std::uint32_t kTracePostActionPairedTailBytes =
    kTracePreviewPlayPairedTailBytes;
static_assert(kTraceCurrentWorldInstallPairedTailBytes == 52u);
static_assert(kTracePreviewPlayPairedTailBytes == 52u);
static_assert(kTracePostActionPairedTailBytes == 52u);

// Record flags.
// Args could not be safely deep-copied (a descriptor pointer was NULL, a
// declared size exceeded this engine's vintage, or an argument-blob bound
// failed its sanity cap). Such a call was necessarily refused by the engine
// with zero side effects, so replay skips the record; the slim record still
// journals that the call happened.
inline constexpr std::uint16_t kTraceFlagArgsUnreplayable = 0x0001;
// On an ApiCall record: the call was issued from callback context — inside
// the enclosing Vn_Update's barrier pair (a notification or log callback
// re-entered the API). The replayer executes such calls after the enclosing
// update returns, which may reorder them relative to the live session's
// mid-update interleaving; it warns, never silently. Exact
// callback-position replay would need the replayer to inject calls from
// inside its own Vn_Update — a recorded leave-open for a future trace
// format version (the mid-state start precedent). On the End record: the
// aggregate — at least one such call exists in this journal.
inline constexpr std::uint16_t kTraceFlagCallbackContext = 0x0002;
inline constexpr std::uint16_t kTraceFlagRawReadableAttempt = 0x0004;

// VTRC v3+ argument encodings are field-wise, fixed-width little-endian. Every
// descriptor begins with a semantic descriptor vintage; pointer values,
// native padding, and native structSize values never enter the journal.
// Pointed-to arrays are serialized row by row in their semantic schema.
//
// Historical VTRC v2 argument encodings, retained by the legacy decoder:
//   u32/u64/f32   raw
//   vec3          3 x f32
//   str           u32 byteLength + bytes (no terminator);
//                 0xFFFFFFFF = the caller passed NULL
//   desc          u32 declaredSize + declaredSize raw struct bytes (pointer
//                 fields travel as garbage and are rebuilt by the replayer),
//                 then the descriptor's pointed-to argument blobs as defined
//                 per call below
//   fileref       str path + u64 fnv1a64(file bytes) + u8 hashKnown (0 when
//                 the file was unreadable at record time — the call was
//                 refused, and the record is flagged unreplayable)
inline constexpr std::uint32_t kTraceStrNull = 0xFFFFFFFFu;

// Sanity caps guarding the recorder's deep copies (argument blobs beyond
// them flag the record unreplayable rather than journaling gigabytes).
inline constexpr std::uint64_t kTraceMaxPcmFloats = 1ull << 28; // 1 GiB
inline constexpr std::uint32_t kTraceMaxContainerChildren = 1u << 20;
inline constexpr std::uint32_t kTraceMaxDescBytes = 1u << 16;

// VTRC v2 call-local extension boundaries. These features were appended
// without changing the container vintage, so their tagged fixed-size tails
// are the only permitted distinction from an older v2 record:
//
//   caller-buffer capture queries:
//     u32 feature, u32 featureVintage, u32 VnResult bits,
//     u32 requiredCount (kTraceRequiredCountUnavailable when undefined)
//
// Capture calls 68/69/77/78 may omit their tail because those call ids
// predate the feature. Capture calls 83--86 were introduced with
// their result tail, so omission is malformed. Any nonempty tail with
// another size, tag, or vintage is refused rather than guessed.
inline constexpr std::uint32_t kTraceCaptureResultFeature =
    0x52504356u;
inline constexpr std::uint32_t kTraceCaptureResultVintage = 1;
inline constexpr std::uint32_t kTraceCaptureResultTailBytes =
    4 + 4 + 4 + 4;
inline constexpr std::uint32_t kTraceRequiredCountUnavailable =
    0xFFFFFFFFu;

// API call ids (u16, pinned — append-only like every wire vocabulary).
// Vn_Init lives in the file header; Vn_TraceStart/Stop are the trace layer
// itself and are not journaled; Vn_Term finalizes the trace implicitly.
enum : std::uint16_t {
    kTraceCallRegisterPCM = 1,     // u64 soundId, desc(VnPCMDesc) +
                                   //   u8 samplesPresent [+ u64 floatCount,
                                   //   floats: interleaved verbatim, or
                                   //   planar planes concatenated]
    kTraceCallUnregisterPCM = 2,   // u64 soundId
    kTraceCallReservedLegacyDirectPlay = 3,
    kTraceCallStop = 4,            // u64 handle, u32 fadeMs
    kTraceCallCreateBus = 5,       // u64 busId, u64 parentId
    kTraceCallSetBusVolume = 6,    // u64 busId, f32 volumeDb, u32 rampMs
    kTraceCallSetVoiceVolume = 7,  // u64 handle, f32 volumeDb, u32 rampMs
    kTraceCallSetVoicePitch = 8,   // u64 handle, f32 ratio
    kTraceCallSetVoicePosition = 9, // u64 handle, vec3
    kTraceCallSetValue = 10,       // u64 variableId, f32 value
    kTraceCallCaptureSnapshot = 11, // u32 structSize
    kTraceCallSetListener = 12,    // vec3 pos, vec3 forward, vec3 up
    kTraceCallRegisterAttenuation = 13, // u64 id, desc(VnAttenuationDesc)
    kTraceCallBindVoiceAttenuation = 14, // u64 handle, u64 attenuationId
    kTraceCallSetVoiceOrientation = 15,  // u64 handle, vec3
    kTraceCallLoadPack = 16,       // desc(VnPackDesc), fileref
    kTraceCallUnloadPack = 17,     // u64 handle
    kTraceCallSetVoiceEffectBypass = 18, // u64 handle, u32 slot, u32 bypass
    kTraceCallSetVoiceEffect = 19, // u64 handle, u32 slot, u32 type,
                                   //   desc(concrete effect desc)
    kTraceCallClearVoiceEffect = 20, // u64 handle, u32 slot
    kTraceCallSetVoiceEffectParam = 21, // u64 handle, u32 slot,
                                        //   u32 paramIndex, f32 value
    kTraceCallRegisterStreamFile = 22, // u64 soundId, fileref
    kTraceCallUnregisterStreamFile = 23, // u64 soundId
    kTraceCallCreateEmitter = 24,  // v4: u64 trace-local emitter ordinal
    kTraceCallDestroyEmitter = 25, // u64 handle
    kTraceCallSetEmitterPosition = 26, // u64 handle, vec3
    kTraceCallSetEmitterOrientation = 27, // u64 handle, vec3
    kTraceCallStopEmitter = 28,    // u64 handle, u32 fadeMs
    kTraceCallSetEmitterValue = 29, // u64 handle, u64 variableId, f32 value
    kTraceCallClearEmitterValue = 30, // u64 handle, u64 variableId
    kTraceCallReservedLegacyContainerRegister = 31,
    kTraceCallReservedLegacyContainerUnregister = 32,
    kTraceCallReservedLegacyActionRegister = 33,
    kTraceCallReservedLegacyActionUnregister = 34,
    kTraceCallReservedLegacyActionTrigger = 35,
    kTraceCallConnectBusSend = 36, // u64 fromBus, u64 toBus, f32 levelDb
    kTraceCallDisconnectBusSend = 37, // u64 fromBus, u64 toBus
    kTraceCallSetBusSendLevel = 38, // u64 fromBus, u64 toBus, f32 levelDb,
                                    //   u32 rampMs
    kTraceCallSetVoiceSend = 39,   // u64 handle, u32 sendSlot, u64 toBus,
                                   //   f32 levelDb
    kTraceCallClearVoiceSend = 40, // u64 handle, u32 sendSlot
    kTraceCallSetVoiceSendLevel = 41, // u64 handle, u32 sendSlot,
                                      //   f32 levelDb, u32 rampMs
    kTraceCallSetBusEffect = 42,   // u64 busId, u32 slot, u32 type,
                                   //   desc(concrete effect desc)
    kTraceCallClearBusEffect = 43, // u64 busId, u32 slot
    kTraceCallSetBusEffectParam = 44, // u64 busId, u32 slot, u32 paramIndex,
                                      //   f32 value
    kTraceCallSetBusEffectBypass = 45, // u64 busId, u32 slot, u32 bypass
    kTraceCallSetBusFaderPoint = 46, // u64 busId, u32 faderAfterSlot
    kTraceCallReservedLegacyActionFindByName = 47,
    kTraceCallReservedLegacyActionTriggerByName = 48,
    kTraceCallReplacePCM = 49,     // u64 soundId, desc(VnPCMDesc) + samples
                                   //   (the RegisterPCM encoding)
    kTraceCallReplaceStreamFile = 50, // u64 soundId, fileref
    kTraceCallReplacePack = 51,    // u64 handle, desc(VnPackDesc), fileref
    kTraceCallReplaceAttenuation = 52, // u64 id, desc(VnAttenuationDesc)
    kTraceCallReservedLegacyActionReplace = 53,
    kTraceCallReservedLegacyContainerReplace = 54,
    kTraceCallSetBusOutputLayout = 55,  // u64 busId, u32 layout
    kTraceCallSetBusDownmixMatrix = 56, // u64 busId, desc(VnDownmixMatrixDesc)
    kTraceCallResetBusDownmixMatrix = 57, // u64 busId
    kTraceCallRegisterBinding = 58, // u64 bindingId, desc(VnBindingDesc) —
                                    //   pointer-free, so the generic desc
                                    //   encoding journals the full payload
                                    //   (curve points included) by value
    kTraceCallUnregisterBinding = 59, // u64 bindingId
    kTraceCallReplaceBinding = 60,  // u64 bindingId, desc(VnBindingDesc)
    kTraceCallAttachBinding = 61,   // u64 bindingId, desc(VnBindingAttachDesc)
    kTraceCallDetachBinding = 62,   // u64 bindingId, desc(VnBindingAttachDesc)
    kTraceCallApplyVoiceBinding = 63, // u64 handle, u64 bindingId
    kTraceCallClearVoiceBinding = 64, // u64 handle, u64 bindingId
    kTraceCallRemoved65 = 65,
    kTraceCallRemoved66 = 66,
    kTraceCallRemoved67 = 67,
    // Caller-buffer capture queries journal their full argument shape so replay
    // reproduces refusals and the truncation counter/log exactly:
    //   u32 capacity, u32 rowStructSize (the caller's declared row
    //   vintage; 0 when the buffer was absent or unread — capacity 0
    //   never reads it), u32 hasRequiredCount (0 = NULL, the refused
    //   shape), then the optional tagged result tail above. Out-rows are not
    //   journaled: replay recomputes them and compares the result code and
    //   every defined requiredCount.
    kTraceCallCapturePropertyTargets = 68,
    kTraceCallCapturePropertyBindings = 69,
    kTraceCallSetBusMute = 70,  // u64 busId, u32 mute01, u32 fadeMs
    kTraceCallSetBusSolo = 71,  // u64 busId, u32 solo01, u32 fadeMs
    kTraceCallReparentBus = 72, // u64 busId, u64 newParentId
    kTraceCallDestroyBus = 73,  // u64 busId
    kTraceCallRemoved74 = 74,
    kTraceCallSetVoiceDistanceScale = 75, // u64 handle, f32 scale
    kTraceCallSetVoiceSpatializationMix = 76, // u64 handle, f32 mix
    kTraceCallCaptureEffectCapabilities = 77, // capture-query shape above
    kTraceCallCaptureEffectParameterCapabilities = 78, // same shape
    kTraceCallSetBusEffectDynamicRoute = 79,
    kTraceCallClearBusEffectDynamicRoute = 80,
    kTraceCallResetVoiceEffect = 81,
    kTraceCallResetBusEffect = 82,
    kTraceCallCaptureEffectDynamicCapabilities = 83,
    kTraceCallCaptureEffectDynamicParameterCapabilities = 84,
    kTraceCallCaptureEffectDynamicRoutes = 85,
    kTraceCallCaptureCompressorRuntimeInfo = 86,
    kTraceCallReservedLegacyActionTriggerEx = 87,
    kTraceCallReservedLegacyActionStop = 88,
    kTraceCallReservedLegacyActionInstanceState = 89,
    kTraceCallStopAll = 90,         // u32 fadeMs
    kTraceCallApplyBusTopology = 91, // deep-copied VnBusTopologyDesc + rows
    kTraceCallSetVoicePan = 92,      // u64 handle, f32 pan
    kTraceCallSetVoiceWidth = 93,    // u64 handle, f32 width
    kTraceCallCommitVoiceStart = 94, // u64 handle
    kTraceCallPreviewPlayPlayable = 95,
    kTraceCallPreviewStopPlayable = 96,
    kTraceCallPreviewQueryPlayable = 97,
    kTraceCallLoadCurrentWorld = 98,
    kTraceCallPostAction = 99,
    kTraceCallStopActionFiring = 100,
    kTraceCallQueryActionFiring = 101,
    kTraceCallQueryActionStep = 102,
    kTraceCallReleaseActionFiring = 103,
    kTraceCallUnloadPackEx = 104, // u64 handle, u32 flags
    kTraceCallPreviewResetPlayable = 105,
    kTraceCallSetVoiceOutputVolume = 106,
    kTraceCallSetBusOutputVolume = 107,
    kTraceCallSetVoiceFilter = 108,
    kTraceCallSetBusFilter = 109,
    kTraceCallSetBusOutputFilter = 110,
    kTraceCallPostActionByName = 128,
    kTraceCallResolveActionByName = 129,
    kTraceCallPostActionPinned = 130,
    kTraceCallPostActionResolved = 131,
    kTraceCallSeekActionFiring = 132,
    kTraceCallQueryActionSeek = 133,
    kTraceCallReleaseActionSeek = 134,
    kTraceCallActivateCanonicalPackSet = 135,
    kTraceCallSetValues = 192,
    kTraceCallSetEmitterValues = 193,
    kTraceCallClearEmitterValues = 194,
    kTraceCallResetValues = 195,
    kTraceCallQueryVariableTicket = 196,
    kTraceCallQueryVariable = 197,
    kTraceCallCreateEmitterEx = 198, // u32 flags, u64 trace-local emitter ordinal
};

enum class TraceCurrentWorldImageTagV1 : std::uint32_t {
    Unavailable = 0,
    RawAttempt = 1,
    CanonicalV6Body = 2,
};

enum class TraceCurrentWorldCaptureDispositionV1 : std::uint32_t {
    None = 0,
    CompleteArtifact = 1,
    HeaderOnlyTooLarge = 2,
    EndOfFileAfterPrefix = 3,
    IoErrorAfterPrefix = 4,
    CapacityFailureAfterPrefix = 5,
    CanonicalBody = 6,
    HeaderOnlySizeMismatch = 7,
};

} // namespace harken

#endif // HARKEN_API_TRACE_FORMAT_H
