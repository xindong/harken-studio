// Logic-thread session-trace recorder: journals the public API call stream
// into a VTRC file (see TraceFormat.h). Owned by the Engine and touched
// only on the externally synchronized logic thread; the audio thread has no
// structural connection to this type (nothing under Source/Runtime/render
// includes it). Writes are buffered in memory and flushed to the file in
// batches on the logic thread — the pack-load file-I/O precedent. A write
// failure stops the trace, marks the engine's sticky traceFailed flag, logs
// a record, and leaves the engine running.
#ifndef HARKEN_API_TRACE_RECORDER_H
#define HARKEN_API_TRACE_RECORDER_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "TraceFormat.h"
#include "harken/core/EngineConfig.h"
#include "harken/param/MeterProbe.h"
#include "harken/param/VariableService.h"
#include "harken/render/TopologyPlan.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <map>
#include <vector>

namespace harken {

// Deterministic recorder allocation failure for internal API boundary tests.
enum class TraceRecorderFault : std::uint32_t {
    None = 0,
    Append = 1,
    ProjectWorldEncode = 2,
    ProjectBufferReserve = 3
};

void SetTraceRecorderFaultForTests(TraceRecorderFault fault) noexcept;
TraceRecorderFault TraceRecorderFaultForTests() noexcept;

struct Engine;
struct PlayableControlState;
class TraceRecorder;
struct ProjectGenerationReservationDomainV1;

class PreparedProjectGenerationTraceV1 {
public:
    PreparedProjectGenerationTraceV1() = default;
    PreparedProjectGenerationTraceV1(const PreparedProjectGenerationTraceV1&) = delete;
    PreparedProjectGenerationTraceV1& operator=(const PreparedProjectGenerationTraceV1&) = delete;
    bool IsPrepared() const noexcept { return prepared_; }

private:
    friend class TraceRecorder;
    void Reset() noexcept {
        domain_.reset();
        recorder_ = nullptr;
        domainEpoch_ = 0;
        traceEpoch_ = 0;
        ordinal_ = 0;
        bufferSize_ = 0;
        requiredEnd_ = 0;
        active_ = false;
        prepared_ = false;
        bytes_.clear();
    }
    std::weak_ptr<ProjectGenerationReservationDomainV1> domain_;
    TraceRecorder* recorder_ = nullptr;
    std::uint64_t domainEpoch_ = 0;
    std::uint64_t traceEpoch_ = 0;
    std::uint64_t ordinal_ = 0;
    std::size_t bufferSize_ = 0;
    std::size_t requiredEnd_ = 0;
    bool active_ = false;
    bool prepared_ = false;
    std::vector<std::uint8_t> bytes_;
};

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

class TraceRecorder {
public:
    explicit TraceRecorder(Engine& engine);
    ~TraceRecorder() noexcept;
    TraceRecorder(const TraceRecorder&) = delete;
    TraceRecorder& operator=(const TraceRecorder&) = delete;

    // Opens the file and writes the VTRC header (semantic config schema plus
    // published base block count). VN_BAD_TRACE_FILE when the file cannot be
    // created or the header cannot be written; the recorder is then inert.
    VnResult Start(const char* utf8Path);

    bool Active() const noexcept { return m_active; }
    std::size_t BufferedBytes() const noexcept { return m_buffer.size(); }
    std::uint64_t NextOrdinal() const noexcept { return m_ordinal; }
    std::uint64_t RecordPackBytes(const void* bytes, std::uint64_t count) noexcept;
    static bool PrepareProjectGenerationTraceV1(
        Engine& engine, const TopologyPlanView& plan,
        const ContainerWorldInstallViewV1& world,
        const WorldInstallResult& result,
        const RuntimeProjectGenerationTokenV1& token,
        std::uint64_t publishedBusFingerprint,
        std::uint64_t compileTargetProfileFingerprint,
        std::uint32_t publicationVintage,
        PreparedProjectGenerationTraceV1* out) noexcept;
    static bool CanCommitProjectGenerationTraceV1(
        const PreparedProjectGenerationTraceV1& token) noexcept;
    static void CommitProjectGenerationTraceV1(
        PreparedProjectGenerationTraceV1* token) noexcept;

    // Finalizes: trailing block barriers, the end record, flush, close.
    // A failure surfaces through the engine's sticky traceFailed flag.
    void Stop() noexcept;

    // Vn_Update barrier pair. Each samples the published block counter and
    // emits the block barriers observed since the previous record first.
    void UpdateBegin() noexcept;
    void UpdateEnd() noexcept;
    void RecordCurrentWorldInstall(
        const ContainerWorldInstallViewV1& install,
        const WorldInstallResult& result, std::uint32_t encoding = 0,
        const RuntimeProjectGenerationTokenV1* pairedToken = nullptr,
        std::uint64_t publishedBusFingerprint = 0,
        std::uint64_t compileTargetProfileFingerprint = 0,
        std::uint32_t publicationVintage = 0) noexcept;
    void RecordProjectGenerationInstallAck(
        const ProjectGenerationInstallAckV1& ack) noexcept;
    void RecordPreparedTopologyPlan(const TopologyPlanView& plan) noexcept;
    void RecordMeterProbeInstall(VnObjectId probeStableId, VnBusId busId,
                                 std::uint32_t tap, std::uint32_t attackMs,
                                 std::uint32_t releaseMs,
                                 std::uint32_t generation,
                                 VnResult expectedStatus) noexcept;
    void RecordMeterProbeUninstall(VnObjectId probeStableId,
                                   std::uint32_t generation,
                                   VnResult expectedStatus) noexcept;
    void RecordMeterProbeBindingDefine(
        VnObjectId bindingId, VnObjectId probeStableId,
        std::uint32_t probeGeneration, std::uint32_t targetScope,
        std::uint32_t targetId, const CurvePoint* points,
        const CurveHandlePair* handles, std::uint32_t pointCount,
        std::uint32_t transitionMs, VnResult expectedStatus) noexcept;
    void RecordMeterProbeBindingUndefine(VnObjectId bindingId,
                                         VnResult expectedStatus) noexcept;
    void RecordMeterProbeBindingAttachment(
        std::uint16_t recordType,
        const MeterProbeAttachment& attachment,
        VnResult expectedStatus) noexcept;
    void RecordVariableOutcome(const VariableOutcomeRecord& outcome) noexcept;
    void RecordSessionTerminate() noexcept;
    void RecordPreviewPlayPlayable(
        bool argsReadable, std::uint32_t structSize, std::uint32_t kind,
        VnObjectId stableId, std::uint64_t emitter, std::uint32_t flags,
        std::uint32_t reserved, std::int32_t result,
        std::uint32_t returnedKind, std::uint64_t handle) noexcept;
    void RecordPreviewResetPlayable(
        bool argsReadable, std::uint32_t structSize, std::uint32_t kind,
        VnObjectId stableId, std::uint64_t emitter, std::uint32_t flags,
        std::uint32_t reserved, std::int32_t result) noexcept;
    void RecordPreviewStopPlayable(std::uint64_t handle,
                                   std::uint32_t fadeMs,
                                   std::int32_t result) noexcept;
    void RecordPreviewQueryPlayable(
        std::uint64_t handle, bool stateReadable, std::uint32_t structSize,
        std::int32_t result, const PlayableControlState* state) noexcept;
    void RememberEmitterHandle(VnEmitterHandle handle) noexcept;
    std::uint64_t RecordedEmitterHandle(VnEmitterHandle handle) const noexcept;

    // Test injection: the next buffered flush reports failure as if the
    // stream write fell short, driving the mid-session write-failure path
    // (sticky traceFailed + log record + engine keeps running) without
    // needing an actual full disk.
    void InjectWriteFailure() noexcept { m_injectWriteFailure = true; }

private:
    friend class TraceCallScribe;

    // Emits block barriers for every published block not yet journaled.
    void SyncBlocks() noexcept;
    void EmitBarrier(std::uint16_t type, std::uint64_t index) noexcept;
    // Begins a record: header appended with payloadBytes = 0, patched at
    // commit. Returns the record's start offset in the buffer.
    std::size_t BeginRecord(std::uint16_t type) noexcept;
    void CommitRecord(std::size_t recordStart) noexcept;
    void SetFlag(std::size_t recordStart, std::uint16_t flag) noexcept;
    void Append(const void* bytes, std::size_t count) noexcept;
    void AppendU16(std::uint16_t value) noexcept;
    void AppendU32(std::uint32_t value) noexcept;
    void AppendU64(std::uint64_t value) noexcept;
    void AppendObjectId(VnObjectId value) noexcept;
    void FlushBuffer() noexcept;
    void FailTrace() noexcept;
    std::uint64_t NowMicros() const;

    Engine& m_engine;
    std::ofstream m_file;
    std::vector<std::uint8_t> m_buffer;
    std::vector<VnEmitterHandle> m_emitterHandles;
    std::vector<std::uint64_t> m_previewHandles;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> m_packResources;
    bool m_active = false;
    bool m_appendFailed = false;
    bool m_injectWriteFailure = false;
    std::uint64_t m_ordinal = 0;
    std::uint64_t m_updateIndex = 0;
    // Nonzero while journaling between an update's begin/end barriers: an
    // API-call record made then was issued from a callback that re-entered
    // the API, and is flagged kTraceFlagCallbackContext. A counter (not a
    // bool) so a callback that itself pumps Vn_Update keeps the context.
    std::uint32_t m_updateDepth = 0;
    // Aggregate for the End record: this journal holds at least one
    // callback-context call.
    bool m_sawCallbackContextCall = false;
    std::uint64_t m_blocksJournaled = 0;
    std::uint64_t m_startTicks = 0;
    std::uint64_t m_ticksPerSecond = 0;
};

// FNV-1a 64 of a file's bytes, streamed on the calling (logic) thread.
// Returns false when the file cannot be read. Shared by the recorder (hash
// at capture) and the replayer (hash re-check before executing a content
// call).
bool HashFileContents(const char* utf8Path, std::uint64_t* outHash);

} // namespace harken

#endif // HARKEN_API_TRACE_RECORDER_H
