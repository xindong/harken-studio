#include "DeviceBackend.h"
#include "DevicePlaybackOpenPlan.h"
#include "RealDeviceInitStage.h"

#include "PlatformBackend.h"
#include "RenderSlicer.h"
#include "harken/core/AllocGuard.h"
#include "harken/render/MixerLoop.h"

#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK) || \
    defined(HARKEN_RT_PROBE_MIX_PUBLICATION) || \
    defined(HARKEN_RT_PROBE_WORKER_BARRIER)
#include "harken/core/RtProbe.h"
#endif
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK)
#include "harken/core/CpuClock.h"
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "miniaudio.h"

#if defined(_WIN32)
extern "C" ma_uint32
VnMiniaudioWasapiPublishedAvrtTaskIndex(ma_device* device) noexcept;
extern "C" ma_bool32
VnMiniaudioAtomicBool32Get(ma_atomic_bool32* value) noexcept;
#endif

#include <cassert>
#include <atomic>
#include <cstdio>
#include <cstring>
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK)
#include <limits>
#endif
#include <memory>
#include <new>
#include <vector>

namespace harken {

namespace {


DeviceStartFault g_deviceStartFault = DeviceStartFault::None;
DeviceBackendTestState* g_deviceBackendTestState = nullptr;

#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK)
template <std::size_t N>
void CopyProbeText(char (&destination)[N], const char* source) noexcept {
    std::size_t i = 0;
    if (source != nullptr) {
        while (i + 1 < N && source[i] != '\0') {
            destination[i] = source[i];
            ++i;
        }
    }
    destination[i] = '\0';
}

std::uint64_t HashProbeBytes(const void* data, std::size_t size) noexcept {
    constexpr std::uint64_t kOffset = 1469598103934665603ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = kOffset;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kPrime;
    }
    return hash;
}
#endif

} // namespace

void SetDeviceStartFaultForTests(DeviceStartFault fault) noexcept {
    g_deviceStartFault = fault;
}

DeviceStartFault DeviceStartFaultForTests() noexcept {
    return g_deviceStartFault;
}

void SetDeviceBackendTestState(DeviceBackendTestState* state) noexcept {
    g_deviceBackendTestState = state;
}

// Render state outlives each platform device instance. A stopped device may
// have consumed only part of the current mixer block, so both the samples and
// cursor must survive until a replacement callback drains the remainder.
struct DeviceBackend::RenderState {
    std::uint32_t blockFrames = 0;
    std::uint32_t channels = 0;
    std::vector<float> staging;
    BlockStagingCursor cursor;
};

// All miniaudio state lives here, behind the header's opaque pointer.
// Teardown flags record how far construction got so the failure path and
// Stop() share one reverse-order cleanup.
struct DeviceBackend::Impl {
    ma_context context{};
    ma_device device{};
    MixerLoop* mixer = nullptr;
    RenderState* renderState = nullptr;
    bool contextReady = false;
    bool deviceReady = false;
    bool started = false;
    bool callbackDomainReady = false;
    std::uint32_t workerCount = 0;
    std::uint64_t domainGeneration = 0;
    std::uint64_t callbackDomainGeneration = 0;
    // Each Impl belongs to exactly one open generation and is destroyed only
    // after its callback has synchronously stopped. A per-Impl gate therefore
    // cannot admit a stale callback from an earlier device generation.
    std::atomic<std::uint32_t> renderAdmission{0};
#if defined(_WIN32)
    std::uint32_t mmcssTaskIndex = 0;
    ma_atomic_bool32 avrtRevertFailedSticky{};
#endif
    bool workerJoined[kMaxRenderWorkerCount]{};
#if defined(_WIN32)
    void* workerAvrtHandles[kMaxRenderWorkerCount]{};
#endif
    DeviceBackendTestState* testState = nullptr;

    static void DataCallback(ma_device* device, void* output,
                             const void* input, ma_uint32 frameCount);
    static RealtimeWorkerJoinResult JoinWorkerDomain(
        void* context, std::uint32_t workerId,
        std::uint64_t generation) noexcept;
    static bool LeaveWorkerDomain(void* context, std::uint32_t workerId,
                                  std::uint64_t generation) noexcept;

    bool AnyWorkerJoined() const noexcept {
        for (std::uint32_t i = 0; i < workerCount; ++i) {
            if (workerJoined[i]) {
                return true;
            }
        }
        return false;
    }

    bool RenderAdmitted() const noexcept {
        return renderAdmission.load(std::memory_order_acquire) != 0;
    }
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "render admission must remain lock-free");

RealtimeWorkerJoinResult DeviceBackend::Impl::JoinWorkerDomain(
    void* context, std::uint32_t workerId,
    std::uint64_t generation) noexcept {
    Impl* impl = static_cast<Impl*>(context);
    if (impl == nullptr || generation == 0 ||
        generation != impl->domainGeneration ||
        workerId >= impl->workerCount || workerId >= kMaxRenderWorkerCount ||
        impl->workerJoined[workerId]) {
        return RealtimeWorkerJoinResult::FailedClean;
    }
    if (impl->testState != nullptr) {
        DeviceBackendTestState& state = *impl->testState;
        ++state.workerJoinCount[workerId];
        state.workerJoinGeneration[workerId] = generation;
        if (state.failWorkerJoinNumber == workerId + 1u) {
            return RealtimeWorkerJoinResult::FailedClean;
        }
        impl->workerJoined[workerId] = true;
        return RealtimeWorkerJoinResult::Joined;
    }
#if defined(_WIN32)
    if (!impl->contextReady || !impl->deviceReady ||
        impl->context.backend != ma_backend_wasapi ||
        impl->context.wasapi.AvSetMmThreadCharacteristicsA == nullptr) {
        return RealtimeWorkerJoinResult::FailedClean;
    }
    using AvSetMmThreadCharacteristicsFn =
        HANDLE(WINAPI*)(LPCSTR, LPDWORD);
    auto setCharacteristics =
        reinterpret_cast<AvSetMmThreadCharacteristicsFn>(
            impl->context.wasapi.AvSetMmThreadCharacteristicsA);
    if (impl->mmcssTaskIndex == 0) {
        return RealtimeWorkerJoinResult::FailedClean;
    }
    DWORD taskIndex = impl->mmcssTaskIndex;
    HANDLE handle = setCharacteristics("Games", &taskIndex);
    if (handle == nullptr) {
        return RealtimeWorkerJoinResult::FailedClean;
    }
    if (taskIndex != impl->mmcssTaskIndex) {
        bool reverted = false;
        if (handle != nullptr &&
            impl->context.wasapi.AvRevertMmThreadcharacteristics != nullptr) {
            using AvRevertMmThreadCharacteristicsFn = BOOL(WINAPI*)(HANDLE);
            auto revertCharacteristics =
                reinterpret_cast<AvRevertMmThreadCharacteristicsFn>(
                    impl->context.wasapi.AvRevertMmThreadcharacteristics);
            reverted = revertCharacteristics(handle) != FALSE;
        }
        if (reverted) {
            return RealtimeWorkerJoinResult::FailedClean;
        }
        // Preserve the handle as an explicit partial membership. The pool
        // treats FailedNeedsLeave as an admission failure but still runs Leave
        // on this same worker during transactional rollback.
        impl->workerAvrtHandles[workerId] = handle;
        impl->workerJoined[workerId] = true;
        return RealtimeWorkerJoinResult::FailedNeedsLeave;
    }
    impl->workerAvrtHandles[workerId] = handle;
    impl->workerJoined[workerId] = true;
    return RealtimeWorkerJoinResult::Joined;
#else
    (void)workerId;
    return RealtimeWorkerJoinResult::FailedClean;
#endif
}

bool DeviceBackend::Impl::LeaveWorkerDomain(
    void* context, std::uint32_t workerId,
    std::uint64_t generation) noexcept {
    Impl* impl = static_cast<Impl*>(context);
    if (impl == nullptr || generation != impl->domainGeneration ||
        workerId >= impl->workerCount || workerId >= kMaxRenderWorkerCount) {
        return false;
    }
    if (!impl->workerJoined[workerId]) {
        return true;
    }
    if (impl->testState != nullptr) {
        DeviceBackendTestState& state = *impl->testState;
        ++state.workerLeaveCount[workerId];
        state.workerLeaveGeneration[workerId] = generation;
        state.workerLeaveSawCallbackStopped[workerId] = !impl->started;
        if (state.failWorkerLeaveCount[workerId] != 0) {
            --state.failWorkerLeaveCount[workerId];
            return false;
        }
        impl->workerJoined[workerId] = false;
        return true;
    }
#if defined(_WIN32)
    HANDLE handle = static_cast<HANDLE>(impl->workerAvrtHandles[workerId]);
    if (handle == nullptr ||
        impl->context.wasapi.AvRevertMmThreadcharacteristics == nullptr) {
        return false;
    }
    using AvRevertMmThreadCharacteristicsFn = BOOL(WINAPI*)(HANDLE);
    auto revertCharacteristics =
        reinterpret_cast<AvRevertMmThreadCharacteristicsFn>(
            impl->context.wasapi.AvRevertMmThreadcharacteristics);
    if (revertCharacteristics(handle) == FALSE) {
        return false;
    }
    impl->workerAvrtHandles[workerId] = nullptr;
    impl->workerJoined[workerId] = false;
    return true;
#else
    return false;
#endif
}

void DeviceBackend::Impl::DataCallback(ma_device* device, void* output,
                                       const void* /*input*/,
                                       ma_uint32 frameCount) {
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK)
    const std::uint64_t probeArrivalTicks = CpuClockTicks();
#endif
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK) || \
    defined(HARKEN_RT_PROBE_MIX_PUBLICATION) || \
    defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    const bool probeWriter = rtprobe::EnterDeviceCallback();
#endif
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK)
    const bool probeGenerationFirst =
        probeWriter && rtprobe::RecordGenerationCallbackBegin(
                           probeArrivalTicks);
    rtprobe::TimelineRecord* probeRecord =
        probeWriter
            ? rtprobe::ReserveTimeline(
                  rtprobe::TimelineKind::DeviceCallback)
            : nullptr;
    std::uint32_t probeMixBlocks = 0;
    if (probeRecord != nullptr) {
        rtprobe::ValidateCoordinator(*probeRecord);
        rtprobe::RecordCallbackArrival(
            *probeRecord, probeArrivalTicks);
        probeRecord->frames = frameCount;
        probeRecord->beginTicks = probeArrivalTicks;
    }
#endif
    Impl* impl = static_cast<Impl*>(device->pUserData);
    float* out = static_cast<float*>(output);
    // Tag the whole callback walk for the allocation guard; the scope is
    // re-entrant (depth-counted), so the mixer's own per-block scope
    // nests cleanly and the copy arithmetic between renders stays
    // tagged. miniaudio's pre/post-callback work stays untagged — the
    // guard polices this engine's code, not the vendor's.
    RenderState& renderState = *impl->renderState;
    float* stagingData = renderState.staging.data();
    const std::uint32_t channels = renderState.channels;
    const bool renderAdmitted = impl->RenderAdmitted();
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK)
    if (probeRecord != nullptr) {
        probeRecord->channels = channels;
        probeRecord->logicalIndex = rtprobe::PackStaging(
            renderState.cursor.readFrames, renderState.cursor.availFrames);
    }
#endif
    {
        memguard::AudioThreadScope audioScope;
        if (!renderAdmitted) {
            // ma_device_start() may synchronously prefill or may let the
            // callback race ahead of logic-thread worker admission. Until the
            // complete open transaction commits, silence the platform buffer
            // without touching mixer or staging state.
            std::memset(
                out, 0,
                static_cast<std::size_t>(frameCount) * channels *
                    sizeof(float));
        } else {
            PumpStagedBlocks(
                renderState.cursor, frameCount, renderState.blockFrames,
                [&] {
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK)
                    ++probeMixBlocks;
#endif
                    impl->mixer->RenderBlock(
                        stagingData, renderState.blockFrames, channels);
                },
                [&](std::uint32_t dstFrames, std::uint32_t srcFrames,
                    std::uint32_t frames) {
                    std::memcpy(
                        out + static_cast<std::size_t>(dstFrames) * channels,
                        stagingData +
                            static_cast<std::size_t>(srcFrames) * channels,
                        static_cast<std::size_t>(frames) * channels *
                            sizeof(float));
                });
        }
    }
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK)
    const std::uint64_t probeEndTicks = CpuClockTicks();
    if (probeRecord != nullptr) {
        probeRecord->relatedIndex = rtprobe::PackStaging(
            renderState.cursor.readFrames, renderState.cursor.availFrames);
        probeRecord->aux0 =
            static_cast<std::uint16_t>(probeMixBlocks);
        if (probeMixBlocks >
            (std::numeric_limits<std::uint16_t>::max)()) {
            probeRecord->flags |= rtprobe::kTimelineFlagMixCountOverflow;
        }
        probeRecord->endTicks = probeEndTicks;
    }
    rtprobe::RecordGenerationCallbackEnd(
        probeGenerationFirst, probeEndTicks);
#endif
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK) || \
    defined(HARKEN_RT_PROBE_MIX_PUBLICATION) || \
    defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    rtprobe::LeaveDeviceCallback();
#endif
}

namespace {

// Maps the platform-neutral backend name (PlatformBackend.h) onto the concrete
// miniaudio backend. Every ma_backend enumerator is declared on every platform;
// only the backend's implementation is platform-guarded inside the vendored
// header, so this switch compiles everywhere and the unused arms cost nothing.
ma_backend ToMaBackend(PlatformAudioBackend backend) {
    switch (backend) {
    case PlatformAudioBackend::Wasapi:
        return ma_backend_wasapi;
    case PlatformAudioBackend::CoreAudio:
        return ma_backend_coreaudio;
    case PlatformAudioBackend::AAudio:
        return ma_backend_aaudio;
    case PlatformAudioBackend::OpenSL:
        return ma_backend_opensl;
    case PlatformAudioBackend::Null:
        break;
    }
    return ma_backend_null;
}

// Writes the compile-time platform backend list into `out` (capacity
// kMaxPlatformBackends) and returns the count (0 on an OUT platform), so the
// context opens exactly the backends this build enables, in preferred-first
// order.
ma_uint32 FillPlatformBackends(ma_backend* out) {
    for (std::size_t i = 0; i < kPlatformBackendCount; ++i) {
        out[i] = ToMaBackend(kPlatformBackends[i]);
    }
    return static_cast<ma_uint32>(kPlatformBackendCount);
}

// The context configuration shared by the channel query and the playback path.
// iOS needs an active AVAudioSession before Core Audio playback is audible. In
// a Unity game there may be no Unity AudioSource to activate the process-wide
// session before Harken starts, so the middleware owns activation for its device
// path. It still avoids deactivation on teardown so it does not shut down a
// session the host may keep using. The desktop-macOS build is unaffected by
// these iOS-only session fields.
ma_context_config MakePlatformContextConfig() {
    ma_context_config config = ma_context_config_init();
#if defined(__APPLE__) && defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
    config.coreaudio.sessionCategory = ma_ios_session_category_playback;
    config.coreaudio.noAudioSessionDeactivate = MA_TRUE;
#endif
    return config;
}

// Stops callback delivery and tears down the platform device while retaining
// the context that owns the render-worker scheduling-domain functions.
VnResult StopImplCallback(DeviceBackend::Impl* impl) {
    if (impl == nullptr) {
        return VN_OK;
    }
    // Revoke before asking the platform to stop. A callback that begins during
    // stop can only write silence and cannot advance the engine timeline.
    impl->renderAdmission.store(0, std::memory_order_release);
    if (impl->testState != nullptr) {
        DeviceBackendTestState* state = impl->testState;
        VnResult result = VN_OK;
        if (impl->started) {
            ++state->stopCount;
        }
        if (impl->started && state->failStopCount != 0) {
            --state->failStopCount;
            result = VN_DEVICE_ERROR;
        }
        impl->started = false;
        impl->deviceReady = false;
        state->callbackRunning = false;
        return result;
    }
    VnResult result = VN_OK;
    if (impl->started) {
        if (ma_device_stop(&impl->device) != MA_SUCCESS) {
            result = VN_DEVICE_ERROR;
        }
        impl->started = false;
    }
    if (impl->deviceReady) {
        // Uninit is the final synchronous join even if the explicit stop
        // reported an error; no callback can survive this operation.
        ma_device_uninit(&impl->device);
        impl->deviceReady = false;
    }
#if defined(_WIN32)
    // The device itself is zeroed by ma_device_uninit(). The backend writes
    // failures into this Impl-owned atomic sink, which survives that zeroing
    // and can be read after the device thread has quiesced.
    if (VnMiniaudioAtomicBool32Get(
            &impl->avrtRevertFailedSticky) != MA_FALSE) {
        result = VN_DEVICE_ERROR;
    }
#endif
    return result;
}

// Destroys the context only after every worker has left (or its thread was
// stopped by the worker-pool rollback). Returning false keeps the context alive
// rather than exposing a stale function pointer to a worker.
bool ReleaseImpl(DeviceBackend::Impl* impl) noexcept {
    if (impl == nullptr) {
        return true;
    }
    if (impl->started || impl->deviceReady || impl->RenderAdmitted()) {
        assert(false && "device domain released before callback exit");
        return false;
    }
    if (impl->testState != nullptr) {
        DeviceBackendTestState* state = impl->testState;
        state->releaseSawAllWorkersLeft = !impl->AnyWorkerJoined();
        ++state->destroyCount;
        delete impl;
        return true;
    }
    if (impl->contextReady) {
        ma_context_uninit(&impl->context);
    }
    delete impl;
    return true;
}

// Full reverse-order teardown for prepare/start failure paths that have not
// exposed a worker domain.
VnResult DestroyImpl(DeviceBackend::Impl* impl) {
    const VnResult result = StopImplCallback(impl);
    (void)ReleaseImpl(impl);
    return result;
}

struct ImplDeleter {
    void operator()(DeviceBackend::Impl* impl) const {
        (void)DestroyImpl(impl);
    }
};

} // namespace

bool QueryDefaultDeviceFormat(DeviceQueryResult* out) {
    if (out == nullptr) {
        return false;
    }
    *out = DeviceQueryResult{};
    if (g_deviceBackendTestState != nullptr) {
        DeviceBackendTestState& state = *g_deviceBackendTestState;
        ++state.queryCount;
        if (!state.querySucceeds) {
            return false;
        }
        out->channels = state.queryChannels;
        out->sampleRate = state.querySampleRate;
        if (out->channels != 0) {
            out->knownFields |= kDeviceQueryChannelsKnown;
        }
        if (out->sampleRate != 0) {
            out->knownFields |= kDeviceQuerySampleRateKnown;
        }
        return true;
    }
    // A build with no real device backend (an OUT platform) never opens a
    // context: the query fails so the caller keeps its fallbacks, exactly as an
    // unavailable device would.
    if (kPlatformBackendCount == 0) {
        return false;
    }
    // A temporary context on the platform's backend(s) (the same list the engine
    // opens) just long enough to read the default playback device's native
    // format; no stream is started. Any failure returns false so the caller
    // keeps its fallbacks.
    ma_context context{};
    ma_backend backends[kMaxPlatformBackends];
    const ma_uint32 backendCount = FillPlatformBackends(backends);
    ma_context_config contextConfig = MakePlatformContextConfig();
    if (ma_context_init(backends, backendCount, &contextConfig, &context) !=
        MA_SUCCESS) {
        return false;
    }
    ma_device_info info{};
    const ma_result result = ma_context_get_device_info(
        &context, ma_device_type_playback, nullptr, &info);
    ma_context_uninit(&context);
    if (result != MA_SUCCESS) {
        return false;
    }
    // The device may report several native formats. Take the widest concrete
    // channel count, and the native sample rate that comes with the widest
    // format (the format the engine would most likely open). A reported 0 for
    // either means "any", which is not concrete: it stays 0 in the result and
    // the caller keeps its own fallback for that field.
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    for (ma_uint32 i = 0; i < info.nativeDataFormatCount; ++i) {
        const ma_uint32 c = info.nativeDataFormats[i].channels;
        if (c > channels) {
            channels = c;
            sampleRate = info.nativeDataFormats[i].sampleRate;
        }
    }
    out->channels = channels;
    out->sampleRate = sampleRate;
    if (channels != 0) {
        out->knownFields |= kDeviceQueryChannelsKnown;
    }
    if (sampleRate != 0) {
        out->knownFields |= kDeviceQuerySampleRateKnown;
    }
    // Succeeds whenever the device info was read; concrete-vs-0 is per field.
    return true;
}

DeviceBackend::~DeviceBackend() {
    Stop();
    delete m_renderState;
}

VnResult DeviceBackend::Prepare(
    MixerLoop& mixer, const EngineConfig& config,
    const DeviceQueryResult* endpointObservation) {
    if (m_impl != nullptr) {
        return VN_OK; // lifecycle calls are idempotent
    }
    DeviceRateObservation rateObservation{};
    rateObservation.requestedDeviceRate = config.mixRate.SampleRate();
    rateObservation.knownFields |= kDeviceRateRequestedKnown;
    if (endpointObservation != nullptr &&
        (endpointObservation->knownFields &
         kDeviceQuerySampleRateKnown) != 0) {
        rateObservation.queriedDefaultEndpointRate =
            endpointObservation->sampleRate;
        rateObservation.knownFields |=
            kDeviceRateQueriedDefaultEndpointKnown;
    }
    m_rateObservation = rateObservation;
    if (DeviceStartFaultForTests() == DeviceStartFault::BeforeBackend) {
        throw std::bad_alloc{};
    }
    if (m_renderState == nullptr) {
        std::unique_ptr<RenderState> renderState(new RenderState());
        renderState->blockFrames = config.blockFrames;
        renderState->channels = config.deviceChannelCount;
        renderState->staging.resize(
            static_cast<std::size_t>(config.blockFrames) *
                config.deviceChannelCount,
            0.0f);
        m_renderState = renderState.release();
        if (g_deviceBackendTestState != nullptr) {
            ++g_deviceBackendTestState->renderStateCreateCount;
        }
    }
    std::unique_ptr<Impl, ImplDeleter> impl(new Impl());
    impl->mixer = &mixer;
    impl->renderState = m_renderState;
    impl->workerCount = config.renderWorkerCount;
    ++m_nextDomainGeneration;
    if (m_nextDomainGeneration == 0) {
        ++m_nextDomainGeneration;
    }
    impl->domainGeneration = m_nextDomainGeneration;

    if (g_deviceBackendTestState != nullptr) {
        impl->testState = g_deviceBackendTestState;
        ++impl->testState->createCount;
        if (impl->testState->failCreateCount != 0) {
            --impl->testState->failCreateCount;
            return VN_DEVICE_ERROR;
        }
        if (impl->testState->openedClientRate != 0) {
            m_rateObservation.openedClientRate =
                impl->testState->openedClientRate;
            m_rateObservation.knownFields |=
                kDeviceRateOpenedClientKnown;
        }
        if (impl->testState->backendInternalRate != 0) {
            m_rateObservation.backendInternalRate =
                impl->testState->backendInternalRate;
            m_rateObservation.knownFields |=
                kDeviceRateBackendInternalKnown;
        }
        impl->deviceReady = true;
        impl->device.pUserData = impl.get();
        impl->testState->openGeneration = impl->domainGeneration;
        m_impl = impl.release();
        return VN_OK;
    }

    // A build with no real device backend (an OUT platform: Linux/other) fails
    // the real-device request here rather than opening anything, so Vn_Init
    // returns VN_DEVICE_ERROR. The engine's own null backend is a separate device
    // mode, never a silent fallback for a requested real device.
    if (kPlatformBackendCount == 0) {
        return VN_DEVICE_ERROR;
    }

#if !defined(_WIN32)
    // Core Audio can replace its AudioUnit and Audio Workgroup during route
    // changes, and Android exposes no equivalent admission API for auxiliary
    // render threads. Until those backends provide a route-generation bridge,
    // a real-device worker request fails closed instead of running unsafe.
    if (config.renderWorkerCount != 0) {
        return VN_DEVICE_ERROR;
    }
#endif

    // The context opens the compile-time platform backend list (platform_backend
    // .h): WASAPI on Windows, Core Audio on macOS/iOS, AAudio with the OpenSL ES
    // fallback on Android. Selection is restricted at runtime rather than by
    // miniaudio's compile-time backend macros, which alter the vendored header's
    // struct layouts and would have to be replicated on every TU that parses it.
    ma_backend backends[kMaxPlatformBackends];
    const ma_uint32 backendCount = FillPlatformBackends(backends);
    ma_context_config contextConfig = MakePlatformContextConfig();
    if (ma_context_init(backends, backendCount, &contextConfig,
                        &impl->context) != MA_SUCCESS) {
        impl.reset();
        SetRealDeviceInitStage("context");
        return VN_DEVICE_ERROR;
    }
    impl->contextReady = true;

    ma_device_config deviceConfig =
        ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = config.deviceChannelCount;
    // The resolved engine rate. When the caller left the public sampleRate 0 on
    // a real backend, Vn_Init has already resolved this to the device's queried
    // native rate before the engine was built, so opening at it honors the
    // device's native rate (spec §7.1); an explicit rate is opened as given and
    // the OS shared-mode mixer converts.
    deviceConfig.sampleRate = config.mixRate.SampleRate();
    // SHARED mode is the engine's device policy (this machine's endpoint
    // serves other applications concurrently; exclusive mode would evict
    // them). miniaudio defaults to shared already — set explicitly so the
    // policy is code, not an inherited default.
    deviceConfig.playback.shareMode = ma_share_mode_shared;
#if defined(_WIN32)
    // Device ownership stays on Harken's externally synchronized lifecycle
    // thread. miniaudio's WASAPI endpoint callback performs blocking device
    // reconstruction, so automatic routing is disabled for every session;
    // hosts switch endpoints through Vn_Suspend followed by Vn_Resume.
    deviceConfig.wasapi.noAutoStreamRouting = MA_TRUE;
    // miniaudio applies this MMCSS task class to the WASAPI callback thread.
    // It is requested only for a parallel session; the default serial device
    // path keeps its prior scheduling policy unchanged.
    if (config.renderWorkerCount != 0) {
        deviceConfig.wasapi.usage = ma_wasapi_usage_games;
    }
#endif
    // The client-side stream buffer is the only slack that rides out
    // stalls of the OS audio delivery thread. Block-sized periods land
    // in WASAPI's low-latency shared path, whose ~2-period buffer
    // (~16 ms at typical endpoint periods) audibly drops audio under
    // real-world ~20 ms scheduler stalls. Request 20 ms periods, double
    // buffered (~40 ms total), the conventional middleware depth; the
    // staging cursor keeps the mixer's block cadence intact for any
    // callback frame count.
    deviceConfig.periodSizeInFrames = config.mixRate.SampleRate() / 50u;
    deviceConfig.periods = 2;
    deviceConfig.dataCallback = &Impl::DataCallback;
    deviceConfig.pUserData = impl.get();
#if defined(__ANDROID__)
    // Tag the AAudio stream as game media so Android's audio policy routes,
    // attenuates, and mixes it as a game rather than a notification or voice
    // stream.
    // Ignored by the OpenSL ES fallback path. (Delegated §7.1 decision.)
    deviceConfig.aaudio.usage = ma_aaudio_usage_game;
    deviceConfig.aaudio.contentType = ma_aaudio_content_type_music;
#endif

    ma_result deviceInit =
        ma_device_init(&impl->context, &deviceConfig, &impl->device);
    PlaybackOpenPlan openPlan = AfterDefaultPlaybackAttempt(
        static_cast<int>(deviceInit));
    if (openPlan.defaultFailed) {
        ma_device_info* playbackInfos = nullptr;
        ma_uint32 playbackCount = 0;
        const ma_result enumerated = ma_context_get_devices(
            &impl->context, &playbackInfos, &playbackCount, nullptr, nullptr);
        if (enumerated != MA_SUCCESS || playbackInfos == nullptr ||
            playbackCount == 0) {
            openPlan = AfterPlaybackEnumerateFailure(openPlan);
            impl.reset();
            SetRealDeviceInitStage("device-init");
            SetRealDeviceInitMaCode(openPlan.maCode);
            SetRealDeviceInitOpenPlan(
                openPlan.enumerateFailed, openPlan.triedEnumeratedCount,
                openPlan.openedEnumeratedIndex);
            return VN_DEVICE_ERROR;
        }
        bool openedEnumerated = false;
        for (ma_uint32 i = 0; i < playbackCount; ++i) {
            ma_device_id chosen = playbackInfos[i].id;
            deviceConfig.playback.pDeviceID = &chosen;
            deviceInit =
                ma_device_init(&impl->context, &deviceConfig, &impl->device);
            openPlan = AfterEnumeratedPlaybackAttempt(
                openPlan, static_cast<int>(i), static_cast<int>(deviceInit),
                static_cast<int>(playbackCount));
            if (openPlan.openedEnumeratedIndex == static_cast<int>(i)) {
                std::fprintf(
                    stderr,
                    "info: default playback endpoint failed; using enumerated "
                    "playback %u %s\n",
                    i,
                    playbackInfos[i].name);
                openedEnumerated = true;
                break;
            }
        }
        if (!openedEnumerated) {
            impl.reset();
            SetRealDeviceInitStage("device-init");
            SetRealDeviceInitMaCode(openPlan.maCode);
            SetRealDeviceInitOpenPlan(
                openPlan.enumerateFailed, openPlan.triedEnumeratedCount,
                openPlan.openedEnumeratedIndex);
            return VN_DEVICE_ERROR;
        }
    }
    impl->deviceReady = true;
#if defined(_WIN32)
    // A parallel session requires miniaudio's callback thread to create and
    // publish a concrete MMCSS task group before workers can join it.
    impl->device.wasapi.requireAvrtTaskGroup =
        config.renderWorkerCount != 0 ? MA_TRUE : MA_FALSE;
    impl->device.wasapi.pAvrtRevertFailedSticky =
        config.renderWorkerCount != 0
            ? &impl->avrtRevertFailedSticky
            : nullptr;
#endif
    if (impl->device.sampleRate != 0) {
        m_rateObservation.openedClientRate = impl->device.sampleRate;
        m_rateObservation.knownFields |= kDeviceRateOpenedClientKnown;
    }
    if (impl->device.playback.internalSampleRate != 0) {
        m_rateObservation.backendInternalRate =
            impl->device.playback.internalSampleRate;
        m_rateObservation.knownFields |=
            kDeviceRateBackendInternalKnown;
    }

#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK)
    rtprobe::BackendObservation observation{};
    observation.valid = 1;
    observation.backendCode =
        static_cast<std::uint32_t>(impl->context.backend);
    observation.clientSampleRate = impl->device.sampleRate;
    observation.clientChannels = impl->device.playback.channels;
    observation.clientFormat =
        static_cast<std::uint32_t>(impl->device.playback.format);
    observation.requestedPeriodFrames = deviceConfig.periodSizeInFrames;
    observation.requestedPeriods = deviceConfig.periods;
    observation.internalSampleRate =
        impl->device.playback.internalSampleRate;
    observation.internalChannels =
        impl->device.playback.internalChannels;
    observation.internalFormat = static_cast<std::uint32_t>(
        impl->device.playback.internalFormat);
    observation.internalPeriodFrames =
        impl->device.playback.internalPeriodSizeInFrames;
    observation.internalPeriods =
        impl->device.playback.internalPeriods;
    observation.conversionOwner =
        rtprobe::kBackendConversionUnknown;
    observation.deviceIdHash = HashProbeBytes(
        &impl->device.playback.id,
        sizeof(impl->device.playback.id));
    const char* backendName = ma_get_backend_name(impl->context.backend);
    CopyProbeText(observation.backendName, backendName);
    CopyProbeText(observation.deviceName, impl->device.playback.name);
    rtprobe::RecordBackendObservation(observation);
#endif
    m_impl = impl.release();
    return VN_OK;
}

VnResult DeviceBackend::StartPrepared(
    std::uint64_t* admissionGeneration) {
    if (admissionGeneration == nullptr) {
        return VN_DEVICE_ERROR;
    }
    *admissionGeneration = 0;
    if (m_impl == nullptr) {
        return VN_DEVICE_ERROR;
    }
    if (m_impl->started) {
        if (!CallbackRealtimeDomainReady()) {
            return VN_DEVICE_ERROR;
        }
        *admissionGeneration = m_impl->domainGeneration;
        return VN_OK;
    }
    if (m_impl->testState != nullptr) {
        DeviceBackendTestState& state = *m_impl->testState;
        ++state.startCount;
        state.startSawAnyWorkerJoined = m_impl->AnyWorkerJoined();
        if (state.failStartCount != 0) {
            --state.failStartCount;
            return VN_DEVICE_ERROR;
        }
        m_impl->started = true;
        state.callbackRunning = true;
        m_impl->callbackDomainGeneration =
            state.callbackDomainGeneration == 0
                ? m_impl->domainGeneration
                : state.callbackDomainGeneration;
        m_impl->callbackDomainReady =
            m_impl->workerCount == 0 ||
            (state.callbackDomainReady &&
             m_impl->callbackDomainGeneration ==
                 m_impl->domainGeneration);
        if (state.pumpCallbackDuringStart) {
            const std::uint64_t requiredSamples =
                static_cast<std::uint64_t>(state.startCallbackFrames) *
                m_impl->renderState->channels;
            if (state.startCallbackFrames == 0 ||
                state.startCallbackOutput == nullptr ||
                requiredSamples > state.startCallbackOutputSamples ||
                !PumpCallbackForTests(state.startCallbackOutput,
                                      state.startCallbackFrames)) {
                return VN_DEVICE_ERROR;
            }
        }
        if (state.failStartAfterCallbackCount != 0) {
            --state.failStartAfterCallbackCount;
            m_impl->started = false;
            state.callbackRunning = false;
            return VN_DEVICE_ERROR;
        }
        if (!m_impl->callbackDomainReady) {
            return VN_DEVICE_ERROR;
        }
        *admissionGeneration = m_impl->domainGeneration;
        return VN_OK;
    }

#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK) || \
    defined(HARKEN_RT_PROBE_MIX_PUBLICATION) || \
    defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    rtprobe::BeginDeviceStartAttempt();
#endif
    if (!m_impl->deviceReady ||
        ma_device_start(&m_impl->device) != MA_SUCCESS) {
        SetRealDeviceInitStage("device-start");
        return VN_DEVICE_ERROR;
    }
    m_impl->started = true;
    m_impl->callbackDomainGeneration = m_impl->domainGeneration;
#if defined(_WIN32)
    m_impl->mmcssTaskIndex =
        VnMiniaudioWasapiPublishedAvrtTaskIndex(&m_impl->device);
    m_impl->callbackDomainReady =
        m_impl->workerCount == 0 ||
        (m_impl->context.backend == ma_backend_wasapi &&
         m_impl->context.wasapi.AvSetMmThreadCharacteristicsA != nullptr &&
         m_impl->context.wasapi.AvRevertMmThreadcharacteristics != nullptr &&
         m_impl->mmcssTaskIndex != 0);
#else
    m_impl->callbackDomainReady = m_impl->workerCount == 0;
#endif
    if (!m_impl->callbackDomainReady) {
        return VN_DEVICE_ERROR;
    }
    *admissionGeneration = m_impl->domainGeneration;
    return VN_OK;
}

bool DeviceBackend::CommitRenderAdmission(
    std::uint64_t admissionGeneration) noexcept {
    if (m_impl == nullptr || admissionGeneration == 0 ||
        admissionGeneration != m_impl->domainGeneration ||
        !CallbackRealtimeDomainReady()) {
        return false;
    }
    // Release publishes the fully prepared callback domain and enabled worker
    // pool to the first callback that observes admission with acquire ordering.
    m_impl->renderAdmission.store(1, std::memory_order_release);
    return true;
}

void DeviceBackend::RevokeRenderAdmission() noexcept {
    if (m_impl != nullptr) {
        m_impl->renderAdmission.store(0, std::memory_order_release);
    }
}

bool DeviceBackend::RenderAdmissionCommitted() const noexcept {
    return m_impl != nullptr && m_impl->RenderAdmitted();
}

RealtimeWorkerDomain DeviceBackend::WorkerDomain() const noexcept {
    RealtimeWorkerDomain domain{};
    if (m_impl == nullptr || m_impl->workerCount == 0 ||
        !CallbackRealtimeDomainReady()) {
        return domain;
    }
    domain.ops.join = &Impl::JoinWorkerDomain;
    domain.ops.leave = &Impl::LeaveWorkerDomain;
    domain.context = m_impl;
    domain.generation = m_impl->domainGeneration;
    return domain;
}

bool DeviceBackend::CallbackRealtimeDomainReady() const noexcept {
    if (!Running()) {
        return false;
    }
    if (m_impl->workerCount == 0) {
        return true;
    }
    if (!m_impl->callbackDomainReady ||
        m_impl->callbackDomainGeneration != m_impl->domainGeneration) {
        return false;
    }
#if defined(_WIN32)
    if (m_impl->testState == nullptr &&
        VnMiniaudioWasapiPublishedAvrtTaskIndex(&m_impl->device) !=
            m_impl->mmcssTaskIndex) {
        return false;
    }
#endif
    return true;
}

bool DeviceBackend::Running() const {
    if (m_impl == nullptr || !m_impl->started) {
        return false;
    }
    if (m_impl->testState != nullptr) {
        return m_impl->testState->callbackRunning;
    }
    return m_impl->deviceReady &&
           ma_device_get_state(&m_impl->device) == ma_device_state_started;
}

VnResult DeviceBackend::StopCallback() {
    return StopImplCallback(m_impl);
}

void DeviceBackend::ReleasePrepared() noexcept {
    Impl* impl = m_impl;
    if (ReleaseImpl(impl)) {
        m_impl = nullptr;
    }
}

VnResult DeviceBackend::Start(
    MixerLoop& mixer, const EngineConfig& config,
    const DeviceQueryResult* endpointObservation) {
    ClearRealDeviceInitStage();
    const VnResult prepare = Prepare(mixer, config, endpointObservation);
    if (prepare != VN_OK) {
        return prepare;
    }
    if (config.renderWorkerCount != 0) {
        // Real-device workers require the engine's bind/start/verify/enable
        // transaction; this convenience wrapper cannot safely supply it.
        (void)StopCallback();
        ReleasePrepared();
        return VN_DEVICE_ERROR;
    }
    std::uint64_t admissionGeneration = 0;
    const VnResult start = StartPrepared(&admissionGeneration);
    if (start != VN_OK ||
        !CommitRenderAdmission(admissionGeneration)) {
        (void)StopCallback();
        ReleasePrepared();
        return VN_DEVICE_ERROR;
    }
    ClearRealDeviceInitStage();
    return VN_OK;
}

VnResult DeviceBackend::Stop() {
    if (m_impl != nullptr && m_impl->AnyWorkerJoined()) {
        // Only the Engine transaction can first disable the pool, quiesce the
        // callback, and run Leave on the original worker threads. Refusing here
        // keeps this convenience surface from invalidating their domain context.
        return VN_DEVICE_ERROR;
    }
    const VnResult result = StopCallback();
    ReleasePrepared();
    return result;
}

bool DeviceBackend::SetRenderStateForTests(
    const float* data, std::uint32_t sampleCount, std::uint32_t readFrames,
    std::uint32_t availFrames) noexcept {
    if (g_deviceBackendTestState == nullptr || m_renderState == nullptr ||
        data == nullptr ||
        static_cast<std::size_t>(sampleCount) !=
            m_renderState->staging.size() ||
        readFrames > m_renderState->blockFrames ||
        availFrames > m_renderState->blockFrames - readFrames) {
        return false;
    }
    std::memcpy(m_renderState->staging.data(), data,
                static_cast<std::size_t>(sampleCount) * sizeof(float));
    m_renderState->cursor.readFrames = readFrames;
    m_renderState->cursor.availFrames = availFrames;
    return true;
}

bool DeviceBackend::GetRenderStateForTests(
    float* data, std::uint32_t sampleCount, std::uint32_t* readFrames,
    std::uint32_t* availFrames) const noexcept {
    if (g_deviceBackendTestState == nullptr || m_renderState == nullptr ||
        data == nullptr || readFrames == nullptr || availFrames == nullptr ||
        static_cast<std::size_t>(sampleCount) !=
            m_renderState->staging.size()) {
        return false;
    }
    std::memcpy(data, m_renderState->staging.data(),
                static_cast<std::size_t>(sampleCount) * sizeof(float));
    *readFrames = m_renderState->cursor.readFrames;
    *availFrames = m_renderState->cursor.availFrames;
    return true;
}

bool DeviceBackend::SetCallbackDomainStateForTests(
    bool started, std::uint64_t callbackDomainGeneration) noexcept {
    if (m_impl == nullptr || m_impl->testState == nullptr ||
        !m_impl->started) {
        return false;
    }
    m_impl->callbackDomainGeneration = callbackDomainGeneration;
    m_impl->callbackDomainReady =
        started &&
        (m_impl->workerCount == 0 ||
         callbackDomainGeneration == m_impl->domainGeneration);
    m_impl->testState->callbackDomainGeneration =
        callbackDomainGeneration;
    m_impl->testState->callbackRunning =
        started &&
        (m_impl->workerCount == 0 || m_impl->callbackDomainReady);
    if (!m_impl->testState->callbackRunning) {
        RevokeRenderAdmission();
    }
    return true;
}

bool DeviceBackend::PumpCallbackForTests(
    float* output, std::uint32_t frameCount) noexcept {
    if (m_impl == nullptr || m_impl->testState == nullptr ||
        !m_impl->started || !m_impl->testState->callbackRunning ||
        output == nullptr || frameCount == 0 ||
        m_impl->device.pUserData != m_impl) {
        return false;
    }
    DeviceBackendTestState& state = *m_impl->testState;
    ++state.callbackAttemptCount;
    if (m_impl->RenderAdmitted()) {
        ++state.callbackAttemptWhileOpenCount;
    } else {
        ++state.callbackAttemptWhileClosedCount;
    }
    Impl::DataCallback(&m_impl->device, output, nullptr, frameCount);
    return true;
}

} // namespace harken
