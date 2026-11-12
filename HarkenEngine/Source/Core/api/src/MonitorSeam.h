#ifndef HARKEN_API_MONITOR_SEAM_H
#define HARKEN_API_MONITOR_SEAM_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include <cstdint>

namespace harken {

struct Engine;
struct MonitorExecutionFact;

namespace monitor {

#if HARKEN_ENGINE_DEV

// Declare before rollback/cleanup guards. Nested scopes publish only after the
// outermost logical operation has fully committed or rolled back.
class IdentityMutationScope {
public:
    IdentityMutationScope() noexcept;
    ~IdentityMutationScope();
    IdentityMutationScope(const IdentityMutationScope&) = delete;
    IdentityMutationScope& operator=(const IdentityMutationScope&) = delete;
private:
    std::uint64_t engine_ = 0;
};

VnResult OnEngineInit(Engine* engine);
void OnEngineTeardown(Engine* engine);
bool InitWouldExceedProcessCap();
bool EventCatalogEnabled();
bool ExecutionObservationEnabled() noexcept;
std::uint64_t ExecutionEngineInstance() noexcept;
bool PublishExecutionFact(MonitorExecutionFact fact, bool audioProducer = false) noexcept;
bool StreamObservationEnabled() noexcept;
bool PublishStreamFact(VnMonitorStreamFactV1 value, bool audioProducer = false) noexcept;
void CaptureStreamIdentity(std::uint32_t ledgerSlot, std::uint64_t render,
                           VnMonitorStreamFactV1& value) noexcept;
void ObserveOperation(std::uint32_t operation, VnResult result,
                      std::uint32_t entityKind, VnObjectId stableId,
                      std::uint64_t handle = 0, std::uint64_t arg0 = 0,
                      std::uint64_t arg1 = 0, float value = 0,
                      const char* text = nullptr);
void ObserveDiagnostic(std::uint32_t level, std::uint32_t code,
                       std::uint64_t arg0, std::uint64_t arg1,
                       std::uint64_t timestampNs, VnObjectId objectId, VnObjectId projectId,
                       const char* text, std::uint32_t logSource = 0,
                       std::uint64_t logTicket = 0);
void CompleteLogTranscription(std::uint32_t source, std::uint64_t consumed);
void OnAudioQuantum();
bool TimeSeriesLiveArmed();
bool AudioLiveArmed();
void PublishTimeSeriesQuantum(std::uint32_t callbackCpuUs,
                              std::uint32_t drainCpuUs,
                              std::uint32_t voicesCpuUs,
                              std::uint32_t busesCpuUs,
                              std::uint32_t arenaHighWaterBytes);
void PublishMasterAudioChunk(const float* interleaved, std::uint32_t frames,
                             std::uint32_t channels, std::uint32_t layout,
                             std::uint32_t sampleRate,
                             std::uint32_t masterGeneration,
                             std::uint64_t sourceFrameBegin);
void PublishPlanarAudioChunk(std::uint32_t tapKind, std::uint64_t ownerHandle,
                             std::uint64_t ownerGeneration, std::uint32_t slotId,
                             float* const* planes, std::uint32_t frames,
                             std::uint32_t channels, std::uint32_t offset,
                             std::uint32_t layout, std::uint32_t sampleRate,
                             std::uint64_t sourceFrameBegin);
void NoteExactInstanceRetired(std::uint32_t tapKind, std::uint64_t ownerHandle,
                              std::uint64_t ownerGeneration);
bool LogicThreadReady();
VnResult GateControlEntry();

#else

class IdentityMutationScope {
public:
    IdentityMutationScope() noexcept = default;
    IdentityMutationScope(const IdentityMutationScope&) = delete;
    IdentityMutationScope& operator=(const IdentityMutationScope&) = delete;
};

inline VnResult OnEngineInit(Engine*) { return VN_OK; }
inline void OnEngineTeardown(Engine*) {}
inline bool InitWouldExceedProcessCap() { return false; }
inline bool EventCatalogEnabled() { return false; }
inline bool ExecutionObservationEnabled() noexcept { return false; }
inline std::uint64_t ExecutionEngineInstance() noexcept { return 0; }
inline bool PublishExecutionFact(const MonitorExecutionFact&, bool = false) noexcept { return false; }
inline bool StreamObservationEnabled() noexcept { return false; }
inline bool PublishStreamFact(const VnMonitorStreamFactV1&, bool = false) noexcept { return false; }
inline void CaptureStreamIdentity(std::uint32_t, std::uint64_t, VnMonitorStreamFactV1&) noexcept {}
inline void ObserveOperation(std::uint32_t, VnResult, std::uint32_t, VnObjectId,
                             std::uint64_t = 0, std::uint64_t = 0,
                             std::uint64_t = 0, float = 0, const char* = nullptr) {}
inline void ObserveDiagnostic(std::uint32_t, std::uint32_t, std::uint64_t, std::uint64_t,
                              std::uint64_t, VnObjectId, VnObjectId, const char*,
                              std::uint32_t = 0, std::uint64_t = 0) {}
inline void CompleteLogTranscription(std::uint32_t, std::uint64_t) {}
inline void OnAudioQuantum() {}
inline bool TimeSeriesLiveArmed() { return false; }
inline bool AudioLiveArmed() { return false; }
inline void PublishTimeSeriesQuantum(std::uint32_t, std::uint32_t, std::uint32_t,
                                     std::uint32_t, std::uint32_t) {}
inline void PublishMasterAudioChunk(const float*, std::uint32_t, std::uint32_t,
                                    std::uint32_t, std::uint32_t, std::uint32_t,
                                    std::uint64_t) {}
inline void PublishPlanarAudioChunk(std::uint32_t, std::uint64_t, std::uint64_t,
                                    std::uint32_t, float* const*, std::uint32_t,
                                    std::uint32_t, std::uint32_t, std::uint32_t,
                                    std::uint32_t, std::uint64_t) {}
inline void NoteExactInstanceRetired(std::uint32_t, std::uint64_t, std::uint64_t) {}
inline bool LogicThreadReady() { return false; }
inline VnResult GateControlEntry() { return VN_OK; }

#endif

} // namespace monitor
} // namespace harken

#endif
