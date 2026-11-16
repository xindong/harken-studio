#ifndef HARKEN_API_TRACE_SEAM_H
#define HARKEN_API_TRACE_SEAM_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "TraceFormat.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace harken {

struct ContainerWorldInstallViewV1;
struct WorldInstallResult;
struct RuntimeProjectGenerationTokenV1;
struct CurvePoint;
struct CurveHandlePair;
struct MeterProbeAttachment;
struct VariableOutcomeRecord;
struct PlayableControlState;
class TraceRecorder;

inline bool IsTransientPackTraceRefusal(VnResult result) noexcept {
    return result == VN_BACKPRESSURE || result == VN_PACK_RESIDENT_CAPACITY ||
        result == VN_NO_PACK || result == VN_NO_BUS || result == VN_DEVICE_ERROR;
}

struct TraceSessionOrigin {
    VnConfig requested{};
    std::uint32_t resolvedSampleRate = 0;
    std::uint32_t resolvedDeviceLayout = VN_CHANNEL_LAYOUT_DEFAULT;
};

struct TraceRecorderDelete {
#if HARKEN_ENGINE_DEV
    void operator()(TraceRecorder* recorder) const noexcept;
#else
    void operator()(TraceRecorder*) const noexcept {}
#endif
};

using TraceRecorderOwner = std::unique_ptr<TraceRecorder, TraceRecorderDelete>;

#if HARKEN_ENGINE_DEV

// Appends one API-call record. Constructed at a public API function's entry
// (a null recorder makes every method a no-op — tracing OFF costs exactly
// the null test); argument methods append in the call's pinned encoding
// order; the destructor commits the record. Deep-copy safety: descriptor
// methods validate the pointer and the declared vintage before copying, and
// bound every pointed-to blob by the format's sanity caps — a failed check
// flags the record unreplayable instead of reading past the caller's
// storage (the engine refuses exactly those calls, with zero side effects,
// so an unreplayable record never hides state).
class TraceCallScribe {
public:
    TraceCallScribe(TraceRecorder* recorder,
                    std::uint16_t callId) noexcept;
    TraceCallScribe(TraceCallScribe&& other) noexcept;
    ~TraceCallScribe() noexcept;
    TraceCallScribe(const TraceCallScribe&) = delete;
    TraceCallScribe& operator=(const TraceCallScribe&) = delete;
    TraceCallScribe& operator=(TraceCallScribe&&) = delete;

    TraceCallScribe& U32(std::uint32_t value) noexcept;
    TraceCallScribe& U64(std::uint64_t value) noexcept;
    TraceCallScribe& ObjectId(VnObjectId value) noexcept;
    TraceCallScribe& F32(float value) noexcept;
    TraceCallScribe& F64(double value) noexcept;
    TraceCallScribe& Vec3(VnVec3 value) noexcept;
    TraceCallScribe& Str(const char* utf8) noexcept;
    TraceCallScribe& Bytes(const void* bytes, std::size_t count) noexcept;
    // u32 structSize read through the caller's pointer (the snapshot-capture
    // record's only argument); NULL flags unreplayable.
    TraceCallScribe& StructSizeOf(const void* descBase) noexcept;
    TraceCallScribe& AttenuationDesc(const VnAttenuationDesc* d) noexcept;
    TraceCallScribe& DownmixMatrixDesc(
        const VnDownmixMatrixDesc* d) noexcept;
    TraceCallScribe& BindingDesc(const VnBindingDesc* d) noexcept;
    TraceCallScribe& BindingAttachDesc(
        const VnBindingAttachDesc* d) noexcept;
    TraceCallScribe& EffectDynamicRouteDesc(
        const VnEffectDynamicRouteDesc* d) noexcept;
    // Concrete effect descriptor selected by type.
    TraceCallScribe& EffectDesc(std::uint32_t type,
                                const VnEffectDesc* d) noexcept;
    TraceCallScribe& PcmDesc(const VnPCMDesc* d) noexcept;
    TraceCallScribe& BusTopologyDesc(
        const VnBusTopologyDesc* d) noexcept;
    // VnPackDesc followed by its path's fileref (path + content hash).
    TraceCallScribe& PackDesc(const VnPackDesc* d) noexcept;
    // A bare path argument's fileref (stream registration surfaces).
    TraceCallScribe& FileRef(const char* utf8Path) noexcept;

    // Only mark calls refused before observable command/state mutation;
    // replay skips the entire call, so this must never hide an applied effect.
    void MarkUnreplayable() noexcept;

private:
    void Append(const void* bytes, std::size_t count) noexcept;
    void AppendU16(std::uint16_t value) noexcept;
    void AppendU32(std::uint32_t value) noexcept;
    void AppendU64(std::uint64_t value) noexcept;
    void AppendObjectId(VnObjectId value) noexcept;
    void AppendFloats(const float* values, std::size_t count) noexcept;
    TraceCallScribe& Scalar32Desc(const void* descBase,
                                 std::uint32_t nativeBytes,
                                 std::uint32_t semanticVintage) noexcept;

    TraceRecorder* m_recorder; // null = tracing off, every method no-ops
    std::size_t m_recordStart = 0;
};

namespace tracing {

bool Active(const TraceRecorder* recorder) noexcept;
void Stop(TraceRecorder* recorder) noexcept;
void UpdateBegin(TraceRecorder* recorder) noexcept;
void UpdateEnd(TraceRecorder* recorder) noexcept;
std::uint64_t RecordPackBytes(TraceRecorder* recorder, const void* bytes,
                              std::uint64_t count) noexcept;
void RecordCurrentWorldInstall(
    TraceRecorder* recorder, const ContainerWorldInstallViewV1& install,
    const WorldInstallResult& result, std::uint32_t encoding = 0,
    const RuntimeProjectGenerationTokenV1* pairedToken = nullptr,
    std::uint64_t publishedBusFingerprint = 0,
    std::uint64_t compileTargetProfileFingerprint = 0,
    std::uint32_t publicationVintage = 0) noexcept;
void RecordMeterProbeInstall(TraceRecorder* recorder, VnObjectId probeStableId,
                             VnBusId busId, std::uint32_t tap,
                             std::uint32_t attackMs, std::uint32_t releaseMs,
                             std::uint32_t generation,
                             VnResult expectedStatus) noexcept;
void RecordMeterProbeUninstall(TraceRecorder* recorder, VnObjectId probeStableId,
                               std::uint32_t generation,
                               VnResult expectedStatus) noexcept;
void RecordMeterProbeBindingDefine(
    TraceRecorder* recorder, VnObjectId bindingId, VnObjectId probeStableId,
    std::uint32_t probeGeneration, std::uint32_t targetScope,
    std::uint32_t targetId, const CurvePoint* points,
    const CurveHandlePair* handles, std::uint32_t pointCount,
    std::uint32_t transitionMs, VnResult expectedStatus) noexcept;
void RecordMeterProbeBindingUndefine(TraceRecorder* recorder, VnObjectId bindingId,
                                     VnResult expectedStatus) noexcept;
void RecordMeterProbeBindingAttachment(TraceRecorder* recorder,
                                       std::uint16_t recordType,
                                       const MeterProbeAttachment& attachment,
                                       VnResult expectedStatus) noexcept;
void RecordVariableOutcome(TraceRecorder* recorder,
                           const VariableOutcomeRecord& outcome) noexcept;
void RecordSessionTerminate(TraceRecorder* recorder) noexcept;
void RecordPreviewPlayPlayable(
    TraceRecorder* recorder, bool argsReadable, std::uint32_t structSize,
    std::uint32_t kind, VnObjectId stableId, std::uint64_t emitter,
    std::uint32_t flags, std::uint32_t reserved, std::int32_t result,
    std::uint32_t returnedKind, std::uint64_t handle) noexcept;
void RecordPreviewResetPlayable(
    TraceRecorder* recorder, bool argsReadable, std::uint32_t structSize,
    std::uint32_t kind, VnObjectId stableId, std::uint64_t emitter,
    std::uint32_t flags, std::uint32_t reserved, std::int32_t result) noexcept;
void RecordPreviewStopPlayable(TraceRecorder* recorder, std::uint64_t handle,
                               std::uint32_t fadeMs, std::int32_t result) noexcept;
void RecordPreviewQueryPlayable(
    TraceRecorder* recorder, std::uint64_t handle, bool stateReadable,
    std::uint32_t structSize, std::int32_t result,
    const PlayableControlState* state) noexcept;
void RememberEmitterHandle(TraceRecorder* recorder, VnEmitterHandle handle) noexcept;
std::uint64_t RecordedEmitterHandle(const TraceRecorder* recorder,
                                    VnEmitterHandle handle) noexcept;

} // namespace tracing

#else

class TraceCallScribe {
public:
    TraceCallScribe(TraceRecorder*, std::uint16_t) noexcept {}
    TraceCallScribe(TraceCallScribe&&) noexcept {}
    TraceCallScribe(const TraceCallScribe&) = delete;
    TraceCallScribe& operator=(const TraceCallScribe&) = delete;
    TraceCallScribe& operator=(TraceCallScribe&&) = delete;

    TraceCallScribe& U32(std::uint32_t) noexcept { return *this; }
    TraceCallScribe& U64(std::uint64_t) noexcept { return *this; }
    TraceCallScribe& ObjectId(VnObjectId) noexcept { return *this; }
    TraceCallScribe& F32(float) noexcept { return *this; }
    TraceCallScribe& F64(double) noexcept { return *this; }
    TraceCallScribe& Vec3(VnVec3) noexcept { return *this; }
    TraceCallScribe& Str(const char*) noexcept { return *this; }
    TraceCallScribe& Bytes(const void*, std::size_t) noexcept { return *this; }
    TraceCallScribe& StructSizeOf(const void*) noexcept { return *this; }
    TraceCallScribe& AttenuationDesc(const VnAttenuationDesc*) noexcept { return *this; }
    TraceCallScribe& DownmixMatrixDesc(const VnDownmixMatrixDesc*) noexcept { return *this; }
    TraceCallScribe& BindingDesc(const VnBindingDesc*) noexcept { return *this; }
    TraceCallScribe& BindingAttachDesc(const VnBindingAttachDesc*) noexcept { return *this; }
    TraceCallScribe& EffectDynamicRouteDesc(const VnEffectDynamicRouteDesc*) noexcept { return *this; }
    TraceCallScribe& EffectDesc(std::uint32_t, const VnEffectDesc*) noexcept { return *this; }
    TraceCallScribe& PcmDesc(const VnPCMDesc*) noexcept { return *this; }
    TraceCallScribe& BusTopologyDesc(const VnBusTopologyDesc*) noexcept { return *this; }
    TraceCallScribe& PackDesc(const VnPackDesc*) noexcept { return *this; }
    TraceCallScribe& FileRef(const char*) noexcept { return *this; }
    void MarkUnreplayable() noexcept {}
};

namespace tracing {

inline bool Active(const TraceRecorder*) noexcept { return false; }
inline void Stop(TraceRecorder*) noexcept {}
inline void UpdateBegin(TraceRecorder*) noexcept {}
inline void UpdateEnd(TraceRecorder*) noexcept {}
inline std::uint64_t RecordPackBytes(TraceRecorder*, const void*, std::uint64_t) noexcept { return 0; }
inline void RecordCurrentWorldInstall(TraceRecorder*, const ContainerWorldInstallViewV1&,
                                      const WorldInstallResult&, std::uint32_t = 0,
                                      const RuntimeProjectGenerationTokenV1* = nullptr,
                                      std::uint64_t = 0, std::uint64_t = 0,
                                      std::uint32_t = 0) noexcept {}
inline void RecordMeterProbeInstall(TraceRecorder*, VnObjectId, VnBusId, std::uint32_t,
                                    std::uint32_t, std::uint32_t, std::uint32_t,
                                    VnResult) noexcept {}
inline void RecordMeterProbeUninstall(TraceRecorder*, VnObjectId, std::uint32_t,
                                      VnResult) noexcept {}
inline void RecordMeterProbeBindingDefine(TraceRecorder*, VnObjectId, VnObjectId,
                                          std::uint32_t, std::uint32_t, std::uint32_t,
                                          const CurvePoint*, const CurveHandlePair*,
                                          std::uint32_t, std::uint32_t, VnResult) noexcept {}
inline void RecordMeterProbeBindingUndefine(TraceRecorder*, VnObjectId, VnResult) noexcept {}
inline void RecordMeterProbeBindingAttachment(TraceRecorder*, std::uint16_t,
                                              const MeterProbeAttachment&, VnResult) noexcept {}
inline void RecordVariableOutcome(TraceRecorder*, const VariableOutcomeRecord&) noexcept {}
inline void RecordSessionTerminate(TraceRecorder*) noexcept {}
inline void RecordPreviewPlayPlayable(TraceRecorder*, bool, std::uint32_t, std::uint32_t,
                                      VnObjectId, std::uint64_t, std::uint32_t,
                                      std::uint32_t, std::int32_t, std::uint32_t,
                                      std::uint64_t) noexcept {}
inline void RecordPreviewResetPlayable(TraceRecorder*, bool, std::uint32_t, std::uint32_t,
                                       VnObjectId, std::uint64_t, std::uint32_t,
                                       std::uint32_t, std::int32_t) noexcept {}
inline void RecordPreviewStopPlayable(TraceRecorder*, std::uint64_t, std::uint32_t,
                                      std::int32_t) noexcept {}
inline void RecordPreviewQueryPlayable(TraceRecorder*, std::uint64_t, bool, std::uint32_t,
                                       std::int32_t, const PlayableControlState*) noexcept {}
inline void RememberEmitterHandle(TraceRecorder*, VnEmitterHandle) noexcept {}
inline std::uint64_t RecordedEmitterHandle(const TraceRecorder*, VnEmitterHandle) noexcept { return 0; }

} // namespace tracing

#endif

} // namespace harken

#endif
