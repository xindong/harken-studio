#include "Engine.h"
#include "HarkenApiInternal.h"

#include <cstdint>
#include <cstring>
#include <type_traits>

struct HarkenSdkTestSpatialSnapshot {
    std::uint64_t firing = 0;
    std::uint64_t instance = 0;
    std::uint64_t emitter = 0;
    std::uint64_t block = 0;
    VnVec3 sourcePosition{};
    VnVec3 sourceForward{};
    VnVec3 listenerPosition{};
    VnVec3 listenerRight{};
};
static_assert(std::is_trivially_copyable_v<HarkenSdkTestSpatialSnapshot>);
static_assert(sizeof(HarkenSdkTestSpatialSnapshot) == 80);

extern "C" __declspec(dllexport) VnResult HarkenSdkTest_CaptureAcceptedSpatial(
    VnActionFiringHandle firing, HarkenSdkTestSpatialSnapshot* output) noexcept {
    if (output == nullptr) return VN_INVALID_ARG;
    *output = {};
    auto* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) return VN_NOT_INITIALIZED;
    VnActionStepResult step{};
    step.structSize = sizeof(step);
    const auto queried = Vn_QueryActionStep(firing, 0, &step);
    if (queried != VN_OK) return queried;
    harken::PlayableRootMetadata metadata{};
    if (step.producedPlayableInstance == 0 ||
        !engine->playableInstances.CaptureRootMetadata(step.producedPlayableInstance, &metadata) ||
        metadata.actionFiring != firing) return VN_STALE_HANDLE;
    HarkenSdkTestSpatialSnapshot captured{};
    captured.firing = firing;
    captured.instance = step.producedPlayableInstance;
    captured.emitter = metadata.invocationEmitter;
    auto& position = captured.sourcePosition;
    auto& forward = captured.sourceForward;
    if (!engine->emitters.Pose(captured.emitter, &position.x, &position.y, &position.z,
                              &forward.x, &forward.y, &forward.z)) return VN_STALE_HANDLE;
    engine->spatial.CaptureListener(captured.listenerPosition, captured.listenerRight);
    captured.block = engine->mixer.BlocksRendered();
    *output = captured;
    return VN_OK;
}

extern "C" __declspec(dllexport) VnResult HarkenSdkTest_PumpNull(std::uint32_t blocks) noexcept {
    auto* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) return VN_NOT_INITIALIZED;
    if (!engine->config.nullDevice) return VN_INVALID_ARG;
    engine->nullBackend.Pump(blocks);
    return VN_OK;
}

extern "C" __declspec(dllexport) VnResult HarkenSdkTest_QueueNotification() noexcept {
    auto* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) return VN_NOT_INITIALIZED;
    VnNotification notification{};
    notification.type = VN_NOTIFY_DEGRADATION_OCCURRED;
    return engine->logicNotifications.Push(notification) ? VN_OK : VN_BACKPRESSURE;
}

extern "C" __declspec(dllexport) VnResult HarkenSdkTest_CaptureTopologyPool(
    std::uint32_t* capacity, std::uint32_t* acquired) noexcept {
    if (capacity != nullptr) *capacity = 0;
    if (acquired != nullptr) *acquired = 0;
    auto* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) return VN_NOT_INITIALIZED;
    if (capacity == nullptr || acquired == nullptr) return VN_INVALID_ARG;
    *capacity = engine->topologyPlans.Capacity();
    for (std::uint32_t index = 0; index < *capacity; ++index) {
        if (engine->topologyPlans.IsAcquired(index)) ++*acquired;
    }
    return VN_OK;
}

extern "C" __declspec(dllexport) VnResult HarkenSdkTest_PumpAndCopy(
    float* output, std::uint32_t capacity, std::uint32_t* outFrames, std::uint32_t* outChannels) noexcept {
    if (outFrames != nullptr) *outFrames = 0;
    if (outChannels != nullptr) *outChannels = 0;
    auto* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) return VN_NOT_INITIALIZED;
    if (!engine->config.nullDevice) return VN_INVALID_ARG;
    const auto frames = engine->nullBackend.BlockFrames();
    const auto channels = engine->nullBackend.Channels();
    const auto sampleCount = static_cast<std::uint64_t>(frames) * channels;
    if (output == nullptr || outFrames == nullptr || outChannels == nullptr || capacity < sampleCount)
        return VN_INVALID_ARG;
    const auto before = engine->mixer.BlocksRendered();
    if (before == UINT64_MAX) return VN_IN_USE;
    engine->nullBackend.Pump(1);
    if (engine->mixer.BlocksRendered() != before + 1u) return VN_IN_USE;
    std::memcpy(output, engine->nullBackend.LastBlock(), sizeof(float) * sampleCount);
    *outFrames = frames;
    *outChannels = channels;
    return VN_OK;
}
