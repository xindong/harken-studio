// End-to-end lifecycle on the null backend: Init, render a run of blocks
// headless, Term. Pass conditions are counter- and result-code-based; the
// silence scan is supplementary and never the sole pass condition (spec
// section 16 silence guard).
#include "Common/ObjectIdTestSupport.h"
#include "UnitTest/AllocationProbe.h"
#include <harken/Harken.h>

#include "Engine.h"
#include "harken/core/AllocGuard.h"
#include "DeviceBackend.h"
#include "../../../HarkenEngine/ThirdParty/miniaudio/miniaudio.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace {

VnConfig MakeNullConfig() {
    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    return config;
}

VnConfig MakeRealConfig() {
    VnConfig config = MakeNullConfig();
    config.deviceMode = VN_DEVICE_REAL;
    config.sampleRate = 48000;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    return config;
}

std::uint32_t ActiveRenderWorkerCount() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot.renderWorkerCount;
}

struct DeviceStartFaultGuard {
    explicit DeviceStartFaultGuard(harken::DeviceStartFault fault) {
        harken::SetDeviceStartFaultForTests(fault);
    }
    ~DeviceStartFaultGuard() {
        harken::SetDeviceStartFaultForTests(harken::DeviceStartFault::None);
    }
    DeviceStartFaultGuard(const DeviceStartFaultGuard&) = delete;
    DeviceStartFaultGuard& operator=(const DeviceStartFaultGuard&) = delete;
};

struct DeviceBackendTestGuard {
    explicit DeviceBackendTestGuard(harken::DeviceBackendTestState& state) {
        harken::SetDeviceBackendTestState(&state);
    }
    ~DeviceBackendTestGuard() {
        if (harken::detail::ActiveEngine() != nullptr) {
            Vn_Term();
        }
        harken::SetDeviceBackendTestState(nullptr);
    }
    DeviceBackendTestGuard(const DeviceBackendTestGuard&) = delete;
    DeviceBackendTestGuard& operator=(const DeviceBackendTestGuard&) = delete;
};

struct MiniaudioStopInjection {
    using StopFn = ma_result (*)(ma_device*);

    StopFn originalStop = nullptr;
    std::thread::id callerThread;
    std::atomic<std::uint32_t> callCount{0};
    std::atomic<bool> ranOnWorker{false};
    std::atomic<int> nextResult{static_cast<int>(MA_ACCESS_DENIED)};
};

ma_result InjectMiniaudioStopResult(ma_device* device) {
    if (device == nullptr || device->pUserData == nullptr) {
        return MA_INVALID_ARGS;
    }
    auto* injection =
        static_cast<MiniaudioStopInjection*>(device->pUserData);
    if (injection->originalStop == nullptr) {
        return MA_INVALID_OPERATION;
    }

    const ma_result backendResult = injection->originalStop(device);
    injection->ranOnWorker.store(
        std::this_thread::get_id() != injection->callerThread,
        std::memory_order_release);
    injection->callCount.fetch_add(1, std::memory_order_release);
    const ma_result injectedResult = static_cast<ma_result>(
        injection->nextResult.exchange(
            static_cast<int>(MA_SUCCESS), std::memory_order_acq_rel));
    return backendResult == MA_SUCCESS && injectedResult != MA_SUCCESS
               ? injectedResult
               : backendResult;
}

void MiniaudioSilenceCallback(ma_device* device, void* output,
                              const void* /*input*/,
                              ma_uint32 frameCount) {
    ma_silence_pcm_frames(output, frameCount, device->playback.format,
                          device->playback.channels);
}

struct MiniaudioNullDeviceGuard {
    ma_context context{};
    ma_device device{};
    MiniaudioStopInjection::StopFn originalStop = nullptr;
    bool contextReady = false;
    bool deviceReady = false;

    ~MiniaudioNullDeviceGuard() {
        if (contextReady && originalStop != nullptr) {
            context.callbacks.onDeviceStop = originalStop;
        }
        if (deviceReady) {
            ma_device_uninit(&device);
        }
        if (contextReady) {
            (void)ma_context_uninit(&context);
        }
    }

    MiniaudioNullDeviceGuard() = default;
    MiniaudioNullDeviceGuard(const MiniaudioNullDeviceGuard&) = delete;
    MiniaudioNullDeviceGuard& operator=(
        const MiniaudioNullDeviceGuard&) = delete;
};

TEST(MiniaudioStopHandoff,
     SynchronousWorkerPublishesExactStopResultAndClearsItOnRestart) {
    MiniaudioNullDeviceGuard scope;
    const ma_backend backend = ma_backend_null;
    ma_context_config contextConfig = ma_context_config_init();
    ASSERT_EQ(MA_SUCCESS,
              ma_context_init(&backend, 1, &contextConfig, &scope.context));
    scope.contextReady = true;

    MiniaudioStopInjection injection;
    injection.originalStop = scope.context.callbacks.onDeviceStop;
    injection.callerThread = std::this_thread::get_id();
    ASSERT_NE(nullptr, injection.originalStop);
    scope.originalStop = injection.originalStop;
    scope.context.callbacks.onDeviceStop = &InjectMiniaudioStopResult;

    ma_device_config deviceConfig =
        ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = 2;
    deviceConfig.sampleRate = 48000;
    deviceConfig.periodSizeInFrames = 64;
    deviceConfig.periods = 2;
    deviceConfig.dataCallback = &MiniaudioSilenceCallback;
    deviceConfig.pUserData = &injection;
    ASSERT_EQ(MA_SUCCESS,
              ma_device_init(&scope.context, &deviceConfig, &scope.device));
    scope.deviceReady = true;

    ASSERT_EQ(MA_SUCCESS, ma_device_start(&scope.device));
    EXPECT_EQ(MA_ACCESS_DENIED, ma_device_stop(&scope.device));
    EXPECT_EQ(ma_device_state_stopped, ma_device_get_state(&scope.device));
    EXPECT_EQ(1u, injection.callCount.load(std::memory_order_acquire));
    EXPECT_TRUE(injection.ranOnWorker.load(std::memory_order_acquire));

    EXPECT_EQ(MA_SUCCESS, ma_device_stop(&scope.device));
    EXPECT_EQ(1u, injection.callCount.load(std::memory_order_acquire));

    ASSERT_EQ(MA_SUCCESS, ma_device_start(&scope.device));
    EXPECT_EQ(MA_SUCCESS, ma_device_stop(&scope.device));
    EXPECT_EQ(ma_device_state_stopped, ma_device_get_state(&scope.device));
    EXPECT_EQ(2u, injection.callCount.load(std::memory_order_acquire));
}

TEST(MonitorSelectionPool, CaptureCopiesCauseForkAndCancellationDoNotAllocate) {
    harken::VoiceLedger ledger(1, 1, 3);
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.AcquirePlayableOwned({0, UINT64_C(1) << 32}, &slot));
    ASSERT_FALSE(allocation_probe::enabled);
    ASSERT_EQ(nullptr, allocation_probe::countIf);
    const auto before = allocation_probe::count;
    harken::MonitorControlCause cause{};
    cause.executionOrdinal = 4;
    allocation_probe::enabled = true;
    bool captured = false;
    bool forked = false;
    {
        harken::VoiceLedger::MonitorSelection original;
        captured = ledger.CaptureMonitorSelection(slot, UINT64_C(1) << 32, &original);
        auto alias = original;
        alias.SetCause(cause);
        harken::MonitorLifecycleFacts facts{};
        forked = alias.CaptureFacts(&facts) && facts.selectionCause.executionOrdinal == 4;
        original = std::move(alias);
        alias = original;
        original = {};
    }
    allocation_probe::enabled = false;
    EXPECT_TRUE(captured);
    EXPECT_TRUE(forked);
    EXPECT_EQ(before, allocation_probe::count);
}

TEST(NullBackendLifecycle, InitRenderBlocksTerm) {
    const VnConfig config = MakeNullConfig();
    ASSERT_FALSE(allocation_probe::enabled);
    ASSERT_EQ(nullptr, allocation_probe::countIf);
    allocation_probe::bytes = 0;
    allocation_probe::enabled = true;
    const VnResult initialized = Vn_Init(&config);
    allocation_probe::enabled = false;
    const auto initializationBytes = allocation_probe::bytes;
    testing::Test::RecordProperty("initialization_bytes", std::to_string(initializationBytes));
    ASSERT_EQ(VN_OK, initialized);
    EXPECT_GT(initializationBytes, 0u);
    // Keep default-capacity preallocation below the 4 GiB scratch regression.
    EXPECT_LE(initializationBytes, 3ull * 1024u * 1024u * 1024u);

    harken::Engine* engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);

    constexpr std::uint32_t kBlocks = 64;
    engine->nullBackend.Pump(kBlocks);

    EXPECT_EQ(kBlocks, engine->mixer.BlocksRendered());

    // Empty mix must be exact digital silence (supplementary assertion).
    const float* block = engine->nullBackend.LastBlock();
    const std::uint32_t samples =
        engine->nullBackend.BlockFrames() * engine->nullBackend.Channels();
    for (std::uint32_t i = 0; i < samples; ++i) {
        ASSERT_EQ(0.0f, block[i]) << "non-silent sample at index " << i;
    }

#ifdef _DEBUG
    // Debug builds must run with the allocation guard armed (spec section 4).
    EXPECT_TRUE(harken::memguard::IsArmed());
#endif
    EXPECT_EQ(0u, harken::memguard::HitCount());

    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(NullBackendLifecycle, ArgumentAndStateErrors) {
    EXPECT_EQ(VN_INVALID_ARG, Vn_Init(nullptr));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_Suspend());
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_Resume());
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_Term());

    VnConfig badSize = MakeNullConfig();
    badSize.structSize = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_Init(&badSize));

    VnConfig badMode = MakeNullConfig();
    badMode.deviceMode = 99;
    EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&badMode));

    const VnConfig good = MakeNullConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&good));
    EXPECT_EQ(VN_ALREADY_INITIALIZED, Vn_Init(&good));
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(NullBackendLifecycle, SuspendResumeAreIdempotentAndPreserveState) {
    const VnConfig config = MakeNullConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&config));

    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    constexpr std::uint32_t kInitialBlocks = 3;
    constexpr std::uint32_t kSuspendedPumpBlocks = 5;
    engine->nullBackend.Pump(kInitialBlocks);
    const std::uint64_t blocksBeforeSuspend =
        engine->mixer.BlocksRendered();

    EXPECT_EQ(VN_OK, Vn_Resume());
    EXPECT_EQ(VN_OK, Vn_Suspend());
    EXPECT_EQ(VN_OK, Vn_Suspend());
    EXPECT_EQ(engine, harken::detail::ActiveEngine());
    engine->nullBackend.Pump(kSuspendedPumpBlocks);
    EXPECT_EQ(blocksBeforeSuspend, engine->mixer.BlocksRendered());

    EXPECT_EQ(VN_OK, Vn_Resume());
    EXPECT_EQ(VN_OK, Vn_Resume());
    EXPECT_EQ(engine, harken::detail::ActiveEngine());
    engine->nullBackend.Pump(1);
    EXPECT_EQ(blocksBeforeSuspend + 1u, engine->mixer.BlocksRendered());
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(NullBackendLifecycle, SuspendThenTermCleansUp) {
    const VnConfig config = MakeNullConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    ASSERT_EQ(VN_OK, Vn_Suspend());
    EXPECT_EQ(VN_OK, Vn_Term());
    EXPECT_EQ(nullptr, harken::detail::ActiveEngine());
}

TEST(NullBackendLifecycle, CallbackFatalStateRefusesSuspendAndResume) {
    const VnConfig config = MakeNullConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    harken::detail::ActiveEngine()->callbackFatalSticky = true;

    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_Suspend());
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_Resume());
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(RealBackendLifecycle, StartFailureRollsBackPublishedEngine) {
    const VnConfig real = MakeRealConfig();

    {
        DeviceStartFaultGuard fault(
            harken::DeviceStartFault::BeforeBackend);
        EXPECT_EQ(VN_DEVICE_ERROR, Vn_Init(&real));
    }

    EXPECT_EQ(nullptr, harken::detail::ActiveEngine());
    EXPECT_FALSE(harken::memguard::IsArmed());

    const VnConfig nullConfig = MakeNullConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&nullConfig));
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(RealBackendLifecycle, SuspendDestroysAndResumeRebuildsDevice) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    const VnConfig real = MakeRealConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&real));

    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    EXPECT_TRUE(engine->device.Running());
    EXPECT_EQ(1u, deviceState.createCount);
    EXPECT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(42), VN_BUS_MASTER));
    EXPECT_EQ(VN_OK, Vn_Resume());
    EXPECT_EQ(1u, deviceState.createCount);

    EXPECT_EQ(VN_OK, Vn_Suspend());
    EXPECT_FALSE(engine->device.Running());
    EXPECT_EQ(1u, deviceState.destroyCount);
    EXPECT_EQ(engine, harken::detail::ActiveEngine());
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(42), -3.0f, 0));

    EXPECT_EQ(VN_OK, Vn_Suspend());
    EXPECT_EQ(1u, deviceState.destroyCount);

    EXPECT_EQ(VN_OK, Vn_Resume());
    EXPECT_TRUE(engine->device.Running());
    EXPECT_EQ(2u, deviceState.createCount);
    EXPECT_EQ(VN_OK, Vn_Resume());
    EXPECT_EQ(2u, deviceState.createCount);

    EXPECT_EQ(VN_OK, Vn_Term());
    EXPECT_EQ(2u, deviceState.destroyCount);
}

TEST(RealBackendLifecycle,
     WorkerDomainStartsSerialAndTracksSuspendResumeGeneration) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    VnConfig real = MakeRealConfig();
    real.renderWorkerCount = 4;
    ASSERT_EQ(VN_OK, Vn_Init(&real));

    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    const std::uint64_t firstGeneration = deviceState.openGeneration;
    ASSERT_NE(0u, firstGeneration);
    EXPECT_EQ(1u, deviceState.startCount);
    EXPECT_FALSE(deviceState.startSawAnyWorkerJoined);
    EXPECT_EQ(4u, engine->voices.RenderWorkerCount());
    EXPECT_EQ(4u, ActiveRenderWorkerCount());
    for (std::uint32_t worker = 0; worker < real.renderWorkerCount; ++worker) {
        EXPECT_EQ(1u, deviceState.workerJoinCount[worker])
            << "worker=" << worker;
        EXPECT_EQ(firstGeneration,
                  deviceState.workerJoinGeneration[worker])
            << "worker=" << worker;
    }

    ASSERT_EQ(VN_OK, Vn_Suspend());
    EXPECT_EQ(0u, engine->voices.RenderWorkerCount());
    EXPECT_EQ(0u, ActiveRenderWorkerCount());
    EXPECT_EQ(1u, deviceState.stopCount);
    EXPECT_EQ(1u, deviceState.destroyCount);
    EXPECT_TRUE(deviceState.releaseSawAllWorkersLeft);
    for (std::uint32_t worker = 0; worker < real.renderWorkerCount; ++worker) {
        EXPECT_EQ(1u, deviceState.workerLeaveCount[worker])
            << "worker=" << worker;
        EXPECT_EQ(firstGeneration,
                  deviceState.workerLeaveGeneration[worker])
            << "worker=" << worker;
        EXPECT_TRUE(deviceState.workerLeaveSawCallbackStopped[worker])
            << "worker=" << worker;
    }

    ASSERT_EQ(VN_OK, Vn_Resume());
    const std::uint64_t secondGeneration = deviceState.openGeneration;
    EXPECT_NE(firstGeneration, secondGeneration);
    EXPECT_EQ(2u, deviceState.startCount);
    EXPECT_FALSE(deviceState.startSawAnyWorkerJoined);
    EXPECT_EQ(4u, engine->voices.RenderWorkerCount());
    EXPECT_EQ(4u, ActiveRenderWorkerCount());
    for (std::uint32_t worker = 0; worker < real.renderWorkerCount; ++worker) {
        EXPECT_EQ(2u, deviceState.workerJoinCount[worker])
            << "worker=" << worker;
        EXPECT_EQ(secondGeneration,
                  deviceState.workerJoinGeneration[worker])
            << "worker=" << worker;
    }

    ASSERT_EQ(VN_OK, Vn_Term());
    EXPECT_EQ(2u, deviceState.stopCount);
    EXPECT_EQ(2u, deviceState.destroyCount);
    EXPECT_TRUE(deviceState.releaseSawAllWorkersLeft);
    for (std::uint32_t worker = 0; worker < real.renderWorkerCount; ++worker) {
        EXPECT_EQ(2u, deviceState.workerLeaveCount[worker])
            << "worker=" << worker;
        EXPECT_EQ(secondGeneration,
                  deviceState.workerLeaveGeneration[worker])
            << "worker=" << worker;
        EXPECT_TRUE(deviceState.workerLeaveSawCallbackStopped[worker])
            << "worker=" << worker;
    }
}

TEST(RealBackendLifecycle,
     StoppedCallbackStateRemainsRetryableAndResumeRebuilds) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    VnConfig real = MakeRealConfig();
    real.renderWorkerCount = 2;
    ASSERT_EQ(VN_OK, Vn_Init(&real));
    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    const std::uint64_t firstGeneration = deviceState.openGeneration;
    ASSERT_NE(0u, firstGeneration);

    ASSERT_TRUE(engine->device.SetCallbackDomainStateForTests(
        true, firstGeneration));
    EXPECT_TRUE(engine->device.Running());
    EXPECT_TRUE(engine->device.CallbackRealtimeDomainReady());
    EXPECT_EQ(2u, ActiveRenderWorkerCount());
    EXPECT_EQ(1u, deviceState.workerJoinCount[0]);
    EXPECT_EQ(1u, deviceState.workerJoinCount[1]);
    EXPECT_EQ(0u, deviceState.workerLeaveCount[0]);
    EXPECT_EQ(0u, deviceState.workerLeaveCount[1]);

    // Device loss can stop the backend outside Vn_Suspend. The public Resume
    // path must detect it and perform a complete device/domain rebuild.
    ASSERT_TRUE(engine->device.SetCallbackDomainStateForTests(
        false, firstGeneration));
    EXPECT_FALSE(engine->suspended);
    EXPECT_FALSE(deviceState.callbackRunning);
    EXPECT_FALSE(engine->device.Running());
    EXPECT_FALSE(engine->device.CallbackRealtimeDomainReady());
    EXPECT_EQ(0u, ActiveRenderWorkerCount());
    EXPECT_EQ(firstGeneration, deviceState.openGeneration);
    EXPECT_EQ(1u, deviceState.createCount);
    EXPECT_EQ(0u, deviceState.destroyCount);
    EXPECT_EQ(0u, deviceState.stopCount);
    EXPECT_EQ(0u, deviceState.workerLeaveCount[0]);
    EXPECT_EQ(0u, deviceState.workerLeaveCount[1]);

    deviceState.callbackDomainGeneration = 0;
    const VnResult resumeResult = Vn_Resume();
    ASSERT_EQ(VN_OK, resumeResult)
        << "create=" << deviceState.createCount
        << " start=" << deviceState.startCount
        << " stop=" << deviceState.stopCount
        << " destroy=" << deviceState.destroyCount
        << " openGeneration=" << deviceState.openGeneration
        << " callbackGeneration="
        << deviceState.callbackDomainGeneration
        << " callbackRunning=" << deviceState.callbackRunning
        << " joins=" << deviceState.workerJoinCount[0] << ','
        << deviceState.workerJoinCount[1]
        << " leaves=" << deviceState.workerLeaveCount[0] << ','
        << deviceState.workerLeaveCount[1];
    const std::uint64_t rebuiltGeneration = deviceState.openGeneration;
    EXPECT_NE(firstGeneration, rebuiltGeneration);
    EXPECT_FALSE(engine->suspended);
    EXPECT_TRUE(engine->device.Running());
    EXPECT_TRUE(engine->device.CallbackRealtimeDomainReady());
    EXPECT_EQ(2u, ActiveRenderWorkerCount());
    EXPECT_EQ(2u, deviceState.createCount);
    EXPECT_EQ(1u, deviceState.destroyCount);
    EXPECT_EQ(2u, deviceState.startCount);
    EXPECT_EQ(1u, deviceState.stopCount);
    EXPECT_TRUE(deviceState.releaseSawAllWorkersLeft);
    EXPECT_EQ(2u, deviceState.workerJoinCount[0]);
    EXPECT_EQ(2u, deviceState.workerJoinCount[1]);
    EXPECT_EQ(rebuiltGeneration, deviceState.workerJoinGeneration[0]);
    EXPECT_EQ(rebuiltGeneration, deviceState.workerJoinGeneration[1]);
    EXPECT_EQ(1u, deviceState.workerLeaveCount[0]);
    EXPECT_EQ(1u, deviceState.workerLeaveCount[1]);
    EXPECT_EQ(firstGeneration, deviceState.workerLeaveGeneration[0]);
    EXPECT_EQ(firstGeneration, deviceState.workerLeaveGeneration[1]);
    EXPECT_TRUE(deviceState.workerLeaveSawCallbackStopped[0]);
    EXPECT_TRUE(deviceState.workerLeaveSawCallbackStopped[1]);
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(RealBackendLifecycle,
     StaleCallbackGenerationFailsInitAndResumeAtomically) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    VnConfig real = MakeRealConfig();
    real.renderWorkerCount = 2;
    deviceState.callbackDomainGeneration = 999;
    EXPECT_EQ(VN_DEVICE_ERROR, Vn_Init(&real));
    EXPECT_EQ(nullptr, harken::detail::ActiveEngine());
    EXPECT_EQ(1u, deviceState.startCount);
    EXPECT_EQ(1u, deviceState.stopCount);
    EXPECT_EQ(1u, deviceState.destroyCount);
    EXPECT_TRUE(deviceState.releaseSawAllWorkersLeft);
    for (std::uint32_t worker = 0; worker < real.renderWorkerCount; ++worker) {
        EXPECT_EQ(0u, deviceState.workerJoinCount[worker])
            << "worker=" << worker;
    }

    deviceState.callbackDomainGeneration = 0;
    ASSERT_EQ(VN_OK, Vn_Init(&real));
    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    const std::uint64_t runningGeneration = deviceState.openGeneration;
    ASSERT_NE(0u, runningGeneration);
    ASSERT_EQ(VN_OK, Vn_Suspend());

    deviceState.callbackDomainGeneration = runningGeneration;
    EXPECT_EQ(VN_DEVICE_ERROR, Vn_Resume());
    const std::uint64_t rejectedGeneration = deviceState.openGeneration;
    EXPECT_NE(runningGeneration, rejectedGeneration);
    EXPECT_EQ(engine, harken::detail::ActiveEngine());
    EXPECT_FALSE(engine->device.Running());
    EXPECT_EQ(0u, engine->voices.RenderWorkerCount());
    EXPECT_EQ(0u, ActiveRenderWorkerCount());
    for (std::uint32_t worker = 0; worker < real.renderWorkerCount; ++worker) {
        EXPECT_EQ(1u, deviceState.workerJoinCount[worker])
            << "worker=" << worker;
    }

    deviceState.callbackDomainGeneration = 0;
    ASSERT_EQ(VN_OK, Vn_Resume());
    EXPECT_NE(rejectedGeneration, deviceState.openGeneration);
    EXPECT_EQ(2u, engine->voices.RenderWorkerCount());
    EXPECT_EQ(2u, ActiveRenderWorkerCount());
    for (std::uint32_t worker = 0; worker < real.renderWorkerCount; ++worker) {
        EXPECT_EQ(2u, deviceState.workerJoinCount[worker])
            << "worker=" << worker;
        EXPECT_EQ(deviceState.openGeneration,
                  deviceState.workerJoinGeneration[worker])
            << "worker=" << worker;
    }
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(RealBackendLifecycle,
     ResumeAdmissionKeepsRacingCallbacksSilentAndFailureAtomic) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    VnConfig real = MakeRealConfig();
    real.renderWorkerCount = 2;
    ASSERT_EQ(VN_OK, Vn_Init(&real));
    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    ASSERT_TRUE(engine->device.RenderAdmissionCommitted());
    ASSERT_EQ(VN_OK, Vn_Suspend());
    ASSERT_FALSE(engine->device.RenderAdmissionCommitted());

    const std::uint32_t blockFrames = engine->config.blockFrames;
    const std::uint32_t channels = engine->config.deviceChannelCount;
    const std::uint32_t sampleCount = blockFrames * channels;
    const std::uint32_t consumedFrames = blockFrames / 4;
    const std::uint32_t remainingFrames = blockFrames - consumedFrames;
    constexpr std::uint32_t kCallbackFrames = 3;
    ASSERT_GT(remainingFrames, kCallbackFrames);

    std::vector<float> staged(sampleCount);
    for (std::uint32_t i = 0; i < sampleCount; ++i) {
        staged[i] = static_cast<float>(i + 1) / 1024.0f;
    }
    ASSERT_TRUE(engine->device.SetRenderStateForTests(
        staged.data(), sampleCount, consumedFrames, remainingFrames));

    std::vector<float> startOutput(kCallbackFrames * channels, 17.0f);
    deviceState.pumpCallbackDuringStart = true;
    deviceState.startCallbackOutput = startOutput.data();
    deviceState.startCallbackOutputSamples =
        static_cast<std::uint32_t>(startOutput.size());
    deviceState.startCallbackFrames = kCallbackFrames;
    deviceState.failWorkerJoinNumber = 1;
    const std::uint64_t blocksBeforeResume = engine->mixer.BlocksRendered();

    EXPECT_EQ(VN_DEVICE_ERROR, Vn_Resume());
    EXPECT_TRUE(engine->suspended);
    EXPECT_FALSE(engine->device.Running());
    EXPECT_FALSE(engine->device.RenderAdmissionCommitted());
    EXPECT_EQ(1u, deviceState.callbackAttemptCount);
    EXPECT_EQ(1u, deviceState.callbackAttemptWhileClosedCount);
    EXPECT_EQ(0u, deviceState.callbackAttemptWhileOpenCount);
    EXPECT_TRUE(std::all_of(startOutput.begin(), startOutput.end(),
                            [](float sample) { return sample == 0.0f; }));
    EXPECT_EQ(blocksBeforeResume, engine->mixer.BlocksRendered());

    std::vector<float> actual(sampleCount, 0.0f);
    std::uint32_t readFrames = 0;
    std::uint32_t availFrames = 0;
    ASSERT_TRUE(engine->device.GetRenderStateForTests(
        actual.data(), sampleCount, &readFrames, &availFrames));
    EXPECT_EQ(staged, actual);
    EXPECT_EQ(consumedFrames, readFrames);
    EXPECT_EQ(remainingFrames, availFrames);

    deviceState.failWorkerJoinNumber = 0;
    std::fill(startOutput.begin(), startOutput.end(), 23.0f);
    ASSERT_EQ(VN_OK, Vn_Resume());
    EXPECT_TRUE(engine->device.RenderAdmissionCommitted());
    EXPECT_EQ(2u, deviceState.callbackAttemptCount);
    EXPECT_EQ(2u, deviceState.callbackAttemptWhileClosedCount);
    EXPECT_EQ(0u, deviceState.callbackAttemptWhileOpenCount);
    EXPECT_TRUE(std::all_of(startOutput.begin(), startOutput.end(),
                            [](float sample) { return sample == 0.0f; }));
    EXPECT_EQ(blocksBeforeResume, engine->mixer.BlocksRendered());
    ASSERT_TRUE(engine->device.GetRenderStateForTests(
        actual.data(), sampleCount, &readFrames, &availFrames));
    EXPECT_EQ(staged, actual);
    EXPECT_EQ(consumedFrames, readFrames);
    EXPECT_EQ(remainingFrames, availFrames);

    std::vector<float> admittedOutput(kCallbackFrames * channels, -1.0f);
    ASSERT_TRUE(engine->device.PumpCallbackForTests(
        admittedOutput.data(), kCallbackFrames));
    for (std::uint32_t i = 0; i < admittedOutput.size(); ++i) {
        EXPECT_EQ(staged[consumedFrames * channels + i],
                  admittedOutput[i]);
    }
    ASSERT_TRUE(engine->device.GetRenderStateForTests(
        actual.data(), sampleCount, &readFrames, &availFrames));
    EXPECT_EQ(consumedFrames + kCallbackFrames, readFrames);
    EXPECT_EQ(remainingFrames - kCallbackFrames, availFrames);
    EXPECT_EQ(blocksBeforeResume, engine->mixer.BlocksRendered());

    const std::uint32_t framesToRender = availFrames + 1;
    std::vector<float> renderOutput(framesToRender * channels, -1.0f);
    ASSERT_TRUE(engine->device.PumpCallbackForTests(
        renderOutput.data(), framesToRender));
    EXPECT_EQ(blocksBeforeResume + 1u, engine->mixer.BlocksRendered());
    EXPECT_EQ(4u, deviceState.callbackAttemptCount);
    EXPECT_EQ(2u, deviceState.callbackAttemptWhileClosedCount);
    EXPECT_EQ(2u, deviceState.callbackAttemptWhileOpenCount);
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(RealBackendLifecycle,
     StartFailureAfterPrefillKeepsResumeStateAndTimelineAtomic) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    const VnConfig real = MakeRealConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&real));
    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    const std::uint64_t initialGeneration = deviceState.openGeneration;
    ASSERT_NE(0u, initialGeneration);
    ASSERT_EQ(VN_OK, Vn_Suspend());

    const std::uint32_t blockFrames = engine->config.blockFrames;
    const std::uint32_t channels = engine->config.deviceChannelCount;
    const std::uint32_t sampleCount = blockFrames * channels;
    constexpr std::uint32_t kConsumedFrames = 7;
    constexpr std::uint32_t kCallbackFrames = 5;
    ASSERT_GT(blockFrames, kConsumedFrames + kCallbackFrames);

    std::vector<float> staged(sampleCount);
    for (std::uint32_t i = 0; i < sampleCount; ++i) {
        staged[i] = static_cast<float>(i + 3) / 2048.0f;
    }
    ASSERT_TRUE(engine->device.SetRenderStateForTests(
        staged.data(), sampleCount, kConsumedFrames,
        blockFrames - kConsumedFrames));

    std::vector<float> startOutput(kCallbackFrames * channels, 31.0f);
    deviceState.pumpCallbackDuringStart = true;
    deviceState.startCallbackOutput = startOutput.data();
    deviceState.startCallbackOutputSamples =
        static_cast<std::uint32_t>(startOutput.size());
    deviceState.startCallbackFrames = kCallbackFrames;
    deviceState.failStartAfterCallbackCount = 1;
    const std::uint64_t blocksBeforeResume = engine->mixer.BlocksRendered();

    EXPECT_EQ(VN_DEVICE_ERROR, Vn_Resume());
    EXPECT_TRUE(engine->suspended);
    EXPECT_FALSE(engine->device.Running());
    EXPECT_FALSE(engine->device.RenderAdmissionCommitted());
    EXPECT_EQ(1u, deviceState.callbackAttemptCount);
    EXPECT_EQ(1u, deviceState.callbackAttemptWhileClosedCount);
    EXPECT_EQ(0u, deviceState.callbackAttemptWhileOpenCount);
    EXPECT_TRUE(std::all_of(startOutput.begin(), startOutput.end(),
                            [](float sample) { return sample == 0.0f; }));
    EXPECT_EQ(blocksBeforeResume, engine->mixer.BlocksRendered());
    VnNotification notification{};
    EXPECT_FALSE(engine->notifications.TryPop(notification));
    EXPECT_FALSE(engine->logicNotifications.TryPop(notification));

    std::vector<float> actual(sampleCount, 0.0f);
    std::uint32_t readFrames = 0;
    std::uint32_t availFrames = 0;
    ASSERT_TRUE(engine->device.GetRenderStateForTests(
        actual.data(), sampleCount, &readFrames, &availFrames));
    EXPECT_EQ(staged, actual);
    EXPECT_EQ(kConsumedFrames, readFrames);
    EXPECT_EQ(blockFrames - kConsumedFrames, availFrames);

    std::fill(startOutput.begin(), startOutput.end(), 47.0f);
    ASSERT_EQ(VN_OK, Vn_Resume());
    EXPECT_FALSE(engine->suspended);
    EXPECT_TRUE(engine->device.RenderAdmissionCommitted());
    EXPECT_NE(initialGeneration, deviceState.openGeneration);
    EXPECT_FALSE(
        engine->device.CommitRenderAdmission(initialGeneration));
    EXPECT_TRUE(engine->device.RenderAdmissionCommitted());
    EXPECT_EQ(2u, deviceState.callbackAttemptCount);
    EXPECT_EQ(2u, deviceState.callbackAttemptWhileClosedCount);
    EXPECT_TRUE(std::all_of(startOutput.begin(), startOutput.end(),
                            [](float sample) { return sample == 0.0f; }));
    EXPECT_EQ(blocksBeforeResume, engine->mixer.BlocksRendered());

    std::vector<float> admittedOutput(kCallbackFrames * channels, -1.0f);
    ASSERT_TRUE(engine->device.PumpCallbackForTests(
        admittedOutput.data(), kCallbackFrames));
    for (std::size_t i = 0; i < admittedOutput.size(); ++i) {
        EXPECT_EQ(staged[kConsumedFrames * channels + i],
                  admittedOutput[i]);
    }
    EXPECT_EQ(3u, deviceState.callbackAttemptCount);
    EXPECT_EQ(2u, deviceState.callbackAttemptWhileClosedCount);
    EXPECT_EQ(1u, deviceState.callbackAttemptWhileOpenCount);
    EXPECT_EQ(blocksBeforeResume, engine->mixer.BlocksRendered());
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(RealBackendLifecycle,
     WorkerLeaveFailureRebuildsFixedPoolAndResumeRemainsRetryable) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    VnConfig real = MakeRealConfig();
    real.renderWorkerCount = 2;
    ASSERT_EQ(VN_OK, Vn_Init(&real));
    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    ASSERT_EQ(2u, engine->voices.RenderWorkerCount());

    deviceState.failWorkerLeaveCount[0] = 1;
    EXPECT_EQ(VN_DEVICE_ERROR, Vn_Suspend());
    EXPECT_EQ(engine, harken::detail::ActiveEngine());
    EXPECT_FALSE(engine->device.Running());
    EXPECT_EQ(0u, engine->voices.RenderWorkerCount());
    EXPECT_EQ(0u, ActiveRenderWorkerCount());
    EXPECT_EQ(1u, deviceState.stopCount);
    EXPECT_EQ(1u, deviceState.destroyCount);
    EXPECT_TRUE(deviceState.releaseSawAllWorkersLeft);
    EXPECT_EQ(2u, deviceState.workerLeaveCount[0]);
    EXPECT_EQ(1u, deviceState.workerLeaveCount[1]);
    EXPECT_TRUE(deviceState.workerLeaveSawCallbackStopped[0]);
    EXPECT_TRUE(deviceState.workerLeaveSawCallbackStopped[1]);

    const std::uint32_t createCountBeforeRecovery = deviceState.createCount;
    ASSERT_EQ(VN_OK, Vn_Resume());
    EXPECT_FALSE(engine->suspended);
    EXPECT_TRUE(engine->device.Running());
    EXPECT_EQ(2u, engine->voices.RenderWorkerCount());
    EXPECT_EQ(2u, ActiveRenderWorkerCount());
    EXPECT_EQ(createCountBeforeRecovery + 1u, deviceState.createCount);
    EXPECT_EQ(2u, deviceState.workerJoinCount[0]);
    EXPECT_EQ(2u, deviceState.workerJoinCount[1]);
    EXPECT_EQ(deviceState.openGeneration,
              deviceState.workerJoinGeneration[0]);
    EXPECT_EQ(deviceState.openGeneration,
              deviceState.workerJoinGeneration[1]);
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(RealBackendLifecycle,
     SuspendResumePreservesPartiallyConsumedStagedBlock) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    const VnConfig real = MakeRealConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&real));

    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    const std::uint32_t blockFrames = engine->config.blockFrames;
    const std::uint32_t channels = engine->config.deviceChannelCount;
    const std::uint32_t sampleCount = blockFrames * channels;
    const std::uint32_t consumedFrames = blockFrames / 2;
    const std::uint32_t remainingFrames = blockFrames - consumedFrames;
    std::vector<float> expected(sampleCount);
    for (std::uint32_t i = 0; i < sampleCount; ++i) {
        expected[i] = static_cast<float>(i + 1) / 1024.0f;
    }
    ASSERT_TRUE(engine->device.SetRenderStateForTests(
        expected.data(), sampleCount, consumedFrames, remainingFrames));
    ASSERT_EQ(1u, deviceState.renderStateCreateCount);

    ASSERT_EQ(VN_OK, Vn_Suspend());
    deviceState.failCreateCount = 1;
    EXPECT_EQ(VN_DEVICE_ERROR, Vn_Resume());
    EXPECT_FALSE(engine->device.Running());

    std::vector<float> actual(sampleCount, 0.0f);
    std::uint32_t readFrames = 0;
    std::uint32_t availFrames = 0;
    ASSERT_TRUE(engine->device.GetRenderStateForTests(
        actual.data(), sampleCount, &readFrames, &availFrames));
    EXPECT_EQ(expected, actual);
    EXPECT_EQ(consumedFrames, readFrames);
    EXPECT_EQ(remainingFrames, availFrames);
    EXPECT_EQ(1u, deviceState.renderStateCreateCount);

    ASSERT_EQ(VN_OK, Vn_Resume());
    std::fill(actual.begin(), actual.end(), 0.0f);
    readFrames = 0;
    availFrames = 0;
    ASSERT_TRUE(engine->device.GetRenderStateForTests(
        actual.data(), sampleCount, &readFrames, &availFrames));
    EXPECT_EQ(expected, actual);
    EXPECT_EQ(consumedFrames, readFrames);
    EXPECT_EQ(remainingFrames, availFrames);
    EXPECT_EQ(1u, deviceState.renderStateCreateCount);
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(RealBackendLifecycle, FailedResumeKeepsEngineForRetry) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    const VnConfig real = MakeRealConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&real));
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(43), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_Suspend());

    harken::Engine* const engine = harken::detail::ActiveEngine();
    deviceState.failCreateCount = 1;
    EXPECT_EQ(VN_DEVICE_ERROR, Vn_Resume());
    EXPECT_EQ(engine, harken::detail::ActiveEngine());
    EXPECT_FALSE(engine->device.Running());
    EXPECT_EQ(2u, deviceState.createCount);
    EXPECT_EQ(2u, deviceState.destroyCount);
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(43), -6.0f, 0));

    EXPECT_EQ(VN_OK, Vn_Resume());
    EXPECT_TRUE(engine->device.Running());
    EXPECT_EQ(3u, deviceState.createCount);
    EXPECT_EQ(VN_OK, Vn_Term());
    EXPECT_EQ(3u, deviceState.destroyCount);
}

TEST(RealBackendLifecycle, SuspendFailureStillLeavesRetryableSuspendedState) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    const VnConfig real = MakeRealConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&real));

    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    const std::uint32_t createCountBeforeSuspend = deviceState.createCount;
    deviceState.failStopCount = 1;
    EXPECT_EQ(VN_DEVICE_ERROR, Vn_Suspend());
    EXPECT_FALSE(engine->device.Running());
    EXPECT_EQ(VN_OK, Vn_Resume());
    EXPECT_TRUE(engine->device.Running());
    EXPECT_EQ(createCountBeforeSuspend + 1u, deviceState.createCount);
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(RealBackendLifecycle, ExternalDeviceStopCanSuspendAndResume) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    const VnConfig real = MakeRealConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&real));

    harken::Engine* const engine = harken::detail::ActiveEngine();
    EXPECT_EQ(VN_OK, engine->device.Stop());
    EXPECT_EQ(1u, deviceState.destroyCount);
    EXPECT_EQ(VN_OK, Vn_Suspend());
    EXPECT_EQ(1u, deviceState.destroyCount);
    EXPECT_EQ(VN_OK, Vn_Resume());
    EXPECT_EQ(2u, deviceState.createCount);
    EXPECT_EQ(VN_OK, Vn_Term());
    EXPECT_EQ(2u, deviceState.destroyCount);
}

TEST(RealBackendLifecycle, DirectDeviceStopRefusesBoundWorkerDomain) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    VnConfig real = MakeRealConfig();
    real.renderWorkerCount = 2;
    ASSERT_EQ(VN_OK, Vn_Init(&real));

    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    EXPECT_EQ(VN_DEVICE_ERROR, engine->device.Stop());
    EXPECT_TRUE(engine->device.Running());
    EXPECT_EQ(0u, deviceState.stopCount);
    EXPECT_EQ(0u, deviceState.destroyCount);
    EXPECT_EQ(2u, engine->voices.RenderWorkerCount());

    EXPECT_EQ(VN_OK, Vn_Suspend());
    EXPECT_FALSE(engine->device.Running());
    EXPECT_EQ(1u, deviceState.stopCount);
    EXPECT_EQ(1u, deviceState.destroyCount);
    EXPECT_TRUE(deviceState.releaseSawAllWorkersLeft);
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(RealBackendLifecycle, TermCleansSuspendedAndRetryFailedStates) {
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);

    const VnConfig real = MakeRealConfig();

    ASSERT_EQ(VN_OK, Vn_Init(&real));
    ASSERT_EQ(VN_OK, Vn_Suspend());
    EXPECT_EQ(VN_OK, Vn_Term());
    EXPECT_EQ(nullptr, harken::detail::ActiveEngine());

    ASSERT_EQ(VN_OK, Vn_Init(&real));
    ASSERT_EQ(VN_OK, Vn_Suspend());
    deviceState.failCreateCount = 1;
    ASSERT_EQ(VN_DEVICE_ERROR, Vn_Resume());
    EXPECT_EQ(VN_OK, Vn_Term());
    EXPECT_EQ(nullptr, harken::detail::ActiveEngine());
    EXPECT_EQ(3u, deviceState.createCount);
    EXPECT_EQ(3u, deviceState.destroyCount);
}

} // namespace
