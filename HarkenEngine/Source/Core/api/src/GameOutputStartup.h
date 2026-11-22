#pragma once

#include "DeviceBackend.h"
#include "RealDeviceInitStage.h"
#include "harken/core/EngineConfig.h"
#include <atomic>
#include <thread>

namespace harken {
#if defined(_WIN32)
// Owned by the control thread. The WASAPI context is opened and released on
// the same lifecycle thread. The callback stays silent until control commits.
class GameOutputStartup {
public:
    ~GameOutputStartup() { Stop(); }

    void Begin(DeviceBackend& device, const EngineConfig& config,
               const DeviceQueryResult* observation) {
        const bool observed = observation != nullptr;
        const DeviceQueryResult format = observed ? *observation : DeviceQueryResult{};
        opening_ = true;
        thread_ = std::thread([this, &device, config, observed, format] {
            VnResult result = VN_DEVICE_ERROR;
            try {
                const auto* endpoint = observed ? &format : nullptr;
                result = device.Prepare(config, endpoint);
                if (result == VN_OK) {
                    attached_.wait(false);
                    if (!close_.load()) {
                        device.BindPreparedMixer(*mixer_);
                        result = device.StartPrepared(&generation_);
                    }
                }
            } catch (...) { result = VN_DEVICE_ERROR; }
            if (result != VN_OK) {
                stage_ = RealDeviceInitStage();
                hasCode_ = RealDeviceInitStageHasCode();
                maCode_ = RealDeviceInitStageCode();
                hasPlan_ = RealDeviceInitStageHasOpenPlan();
                enumerateFailed_ = RealDeviceInitEnumerateFailed();
                tried_ = RealDeviceInitTriedEnumeratedCount();
                opened_ = RealDeviceInitOpenedEnumeratedIndex();
            }
            result_.store(result, std::memory_order_release);
            result_.notify_one();
            close_.wait(false);
            stopResult_ = device.Stop();
        });
    }

    void Attach(MixerLoop& mixer) noexcept {
        mixer_ = &mixer;
        attached_.store(true);
        attached_.notify_one();
    }

    bool Opening() const noexcept { return opening_; }
    bool OwnsDevice() const noexcept { return thread_.joinable(); }
    VnResult Finish(DeviceBackend& device, bool wait = false) noexcept {
        if (wait) result_.wait(VN_IN_USE);
        const auto result = result_.load(std::memory_order_acquire);
        if (result != VN_OK) {
            if (result != VN_IN_USE) {
                SetRealDeviceInitStage(stage_);
                if (hasCode_) SetRealDeviceInitMaCode(maCode_);
                if (hasPlan_) SetRealDeviceInitOpenPlan(enumerateFailed_, tried_, opened_);
            }
            return result;
        }
        if (!device.CommitRenderAdmission(generation_)) return VN_DEVICE_ERROR;
        opening_ = false;
        return VN_OK;
    }
    VnResult Stop() noexcept {
        if (thread_.joinable()) {
            close_.store(true);
            attached_.store(true);
            attached_.notify_one();
            close_.notify_one();
            thread_.join();
        }
        opening_ = false;
        return stopResult_;
    }
private:
    std::thread thread_;
    std::atomic<bool> attached_{false};
    MixerLoop* mixer_ = nullptr;
    const char* stage_ = nullptr;
    bool hasCode_ = false, hasPlan_ = false, enumerateFailed_ = false;
    int maCode_ = 0, tried_ = 0, opened_ = -1;
    std::atomic<bool> opening_{false};
    std::atomic<bool> close_{false};
    std::atomic<VnResult> result_{VN_IN_USE};
    std::uint64_t generation_{};
    VnResult stopResult_ = VN_OK;
};
#else
// Other backends open their device directly and have no pending preparation.
class GameOutputStartup {
public:
    constexpr bool Opening() const noexcept { return false; }
    constexpr bool OwnsDevice() const noexcept { return false; }
    constexpr VnResult Finish(DeviceBackend&) noexcept { return VN_CAPABILITY_UNAVAILABLE; }
    constexpr VnResult Stop() noexcept { return VN_OK; }
};
#endif

// The backend address stays stable from endpoint activation to final shutdown.
struct GameOutputResources {
    DeviceBackend device;
    GameOutputStartup startup;
};
}
