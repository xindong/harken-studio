// Real audio device backend: owns the OS output device whose data
// callback IS the mixer loop (no intermediate thread). The backend is
// selected at compile time per platform (PlatformBackend.h): WASAPI on
// Windows, Core Audio on macOS/iOS, AAudio with the OpenSL ES fallback on
// Android. Device policy is shared-mode, non-exclusive on every platform —
// other audio applications keep running; the engine never takes exclusive
// ownership of the endpoint.
//
// The implementation hides behind a pointer so miniaudio's header stays
// confined to the backend's own translation unit: every consumer of the
// engine object (tests included) would otherwise parse the entire
// vendored header. Construction of the implementation happens at
// Vn_Init time on the logic thread, where allocation is permitted; the
// device callback path allocates nothing.
#ifndef HARKEN_API_DEVICE_BACKEND_H
#define HARKEN_API_DEVICE_BACKEND_H

#include <harken/Harken.h>

#include "harken/core/EngineConfig.h"
#include "harken/render/RenderWorkerPool.h"

namespace harken {

// Deterministic startup failure for internal API boundary tests.
enum class DeviceStartFault : std::uint32_t {
    None = 0,
    BeforeBackend = 1
};

void SetDeviceStartFaultForTests(DeviceStartFault fault) noexcept;
DeviceStartFault DeviceStartFaultForTests() noexcept;

inline constexpr std::uint32_t kDeviceRateRequestedKnown = 1u << 0;
inline constexpr std::uint32_t kDeviceRateOpenedClientKnown = 1u << 1;
inline constexpr std::uint32_t kDeviceRateBackendInternalKnown = 1u << 2;
inline constexpr std::uint32_t kDeviceRateQueriedDefaultEndpointKnown = 1u << 3;

// Latest real-device open attempt. The queried default endpoint may change
// before the backend opens it, so unequal known rates do not prove conversion
// or identify which layer owns it.
struct DeviceRateObservation {
    std::uint32_t requestedDeviceRate = 0;
    std::uint32_t openedClientRate = 0;
    std::uint32_t backendInternalRate = 0;
    std::uint32_t queriedDefaultEndpointRate = 0;
    std::uint32_t knownFields = 0;
};

// Internal lifecycle seam. When installed, device creation and destruction are
// counted without opening a platform endpoint; requested failures remain fully
// deterministic. This header is private to the Runtime implementation/tests.
struct DeviceBackendTestState {
    std::uint32_t createCount = 0;
    std::uint32_t destroyCount = 0;
    std::uint32_t startCount = 0;
    std::uint32_t stopCount = 0;
    std::uint32_t renderStateCreateCount = 0;
    std::uint32_t failCreateCount = 0;
    std::uint32_t failStartCount = 0;
    // Fails after the optional synchronous callback below, modeling backends
    // such as OpenSL ES that prefill before a later start/enqueue failure.
    std::uint32_t failStartAfterCallbackCount = 0;
    std::uint32_t failStopCount = 0;
    std::uint32_t queryCount = 0;
    bool querySucceeds = false;
    std::uint32_t queryChannels = 0;
    std::uint32_t querySampleRate = 0;
    std::uint32_t openedClientRate = 0;
    std::uint32_t backendInternalRate = 0;
    // A zero failWorkerJoinNumber means no injected failure; otherwise the
    // one-based worker number fails each Join until the test clears it.
    std::uint32_t failWorkerJoinNumber = 0;
    bool callbackDomainReady = true;
    // Models the actual backend callback state independently of the lifecycle
    // owner's open intent, including device loss after Start succeeded.
    bool callbackRunning = true;
    // Zero selects the current open generation. A non-zero value lets tests
    // present a stale callback-domain acknowledgement.
    std::uint64_t callbackDomainGeneration = 0;
    std::uint64_t openGeneration = 0;
    bool startSawAnyWorkerJoined = false;
    bool releaseSawAllWorkersLeft = false;
    std::uint32_t workerJoinCount[kMaxRenderWorkerCount]{};
    std::uint32_t workerLeaveCount[kMaxRenderWorkerCount]{};
    std::uint32_t failWorkerLeaveCount[kMaxRenderWorkerCount]{};
    std::uint64_t workerJoinGeneration[kMaxRenderWorkerCount]{};
    std::uint64_t workerLeaveGeneration[kMaxRenderWorkerCount]{};
    bool workerLeaveSawCallbackStopped[kMaxRenderWorkerCount]{};
    // When enabled, the fake StartPrepared path invokes one synchronous data
    // callback before returning, matching backends that prefill or begin
    // delivery inside ma_device_start(). The test owns the output storage.
    bool pumpCallbackDuringStart = false;
    float* startCallbackOutput = nullptr;
    std::uint32_t startCallbackOutputSamples = 0;
    std::uint32_t startCallbackFrames = 0;
    std::uint32_t callbackAttemptCount = 0;
    std::uint32_t callbackAttemptWhileClosedCount = 0;
    std::uint32_t callbackAttemptWhileOpenCount = 0;
};

void SetDeviceBackendTestState(DeviceBackendTestState* state) noexcept;

// The default playback device's native format, as read by
// QueryDefaultDeviceFormat. A field left 0 means "the platform reported no
// concrete value" (e.g. a device that accepts any rate/width); the caller keeps
// its own fallback for that field.
struct DeviceQueryResult {
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t knownFields = 0;
};

inline constexpr std::uint32_t kDeviceQueryChannelsKnown = 1u << 0;
inline constexpr std::uint32_t kDeviceQuerySampleRateKnown = 1u << 1;

// Queries the default playback device's native channel count and sample rate
// through a temporary miniaudio context (no stream is opened). Returns true and
// fills *out on success (each field 0 where the platform reports no concrete
// value); false when no default device can be queried, on any backend error, or
// on a build that enables no real backend (an OUT platform) — the caller then
// keeps its fallbacks. Realizes the "follow the device's native layout / rate"
// contract during Vn_Init when deviceChannelLayout == VN_CHANNEL_LAYOUT_DEFAULT
// and/or the public sampleRate == 0. Resume queries update observation only.
// Logic thread, immediately before each real-device open. The real-device path
// is validated manually; CI uses the deterministic seam below.
bool QueryDefaultDeviceFormat(DeviceQueryResult* out);

class MixerLoop;

class DeviceBackend {
public:
    DeviceBackend() = default;
    ~DeviceBackend(); // tears down device and persistent render staging

    DeviceBackend(const DeviceBackend&) = delete;
    DeviceBackend& operator=(const DeviceBackend&) = delete;

    // Prepare builds the context and playback device without starting callback
    // delivery. StartPrepared starts and verifies the callback scheduling
    // domain; workers remain disabled until the Engine-level transaction binds
    // them. StopCallback synchronously ends callback delivery and destroys the
    // device while preserving the context for worker Leave. ReleasePrepared is
    // valid only after the worker pool has quiesced its domain.
    VnResult Prepare(MixerLoop& mixer, const EngineConfig& config,
                     const DeviceQueryResult* endpointObservation);
    VnResult StartPrepared(std::uint64_t* admissionGeneration);
    bool CommitRenderAdmission(std::uint64_t admissionGeneration) noexcept;
    void RevokeRenderAdmission() noexcept;
    VnResult StopCallback();
    void ReleasePrepared() noexcept;
    RealtimeWorkerDomain WorkerDomain() const noexcept;
    bool CallbackRealtimeDomainReady() const noexcept;
    bool RenderAdmissionCommitted() const noexcept;

    VnResult Start(MixerLoop& mixer, const EngineConfig& config,
                   const DeviceQueryResult* endpointObservation);

    // Complete zero-worker convenience wrappers. A parallel real-device
    // session must use the Engine-level two-phase transaction above.
    VnResult Stop();

    bool Prepared() const { return m_impl != nullptr; }
    bool Running() const;
    DeviceRateObservation RateObservation() const noexcept {
        return m_rateObservation;
    }

    // Internal deterministic seam for lifecycle tests. Both operations run on
    // the logic thread while the fake device seam has no callback.
    bool SetRenderStateForTests(const float* data, std::uint32_t sampleCount,
                                std::uint32_t readFrames,
                                std::uint32_t availFrames) noexcept;
    bool GetRenderStateForTests(float* data, std::uint32_t sampleCount,
                                 std::uint32_t* readFrames,
                                 std::uint32_t* availFrames) const noexcept;
    bool SetCallbackDomainStateForTests(
        bool started, std::uint64_t callbackDomainGeneration) noexcept;
    bool PumpCallbackForTests(float* output,
                              std::uint32_t frameCount) noexcept;

    struct Impl; // public so the C data callback can name it
    struct RenderState; // persistent staging owned across device rebuilds

private:
    Impl* m_impl = nullptr;
    RenderState* m_renderState = nullptr;
    DeviceRateObservation m_rateObservation;
    std::uint64_t m_nextDomainGeneration = 0;
};

} // namespace harken

#endif // HARKEN_API_DEVICE_BACKEND_H
