#pragma once

#include "DeviceBackend.h"
#include <atomic>
#include <thread>

namespace harken {
// Owned by the control thread. The WASAPI context is opened and released on
// the same lifecycle thread. The callback stays silent until control commits.
class GameOutputStartup {
public:
    ~GameOutputStartup() { Stop(); }

    void Begin(DeviceBackend& device, MixerLoop& mixer, const EngineConfig& config,
               const DeviceQueryResult* observation) {
        const bool observed = observation != nullptr;
        const DeviceQueryResult format = observed ? *observation : DeviceQueryResult{};
        opening_ = true;
        thread_ = std::thread([this, &device, &mixer, &config, observed, format] {
            VnResult result = VN_DEVICE_ERROR;
            try {
                const auto* endpoint = observed ? &format : nullptr;
                result = device.Prepare(mixer, config, endpoint);
                if (result == VN_OK) result = device.StartPrepared(&generation_);
            } catch (...) { result = VN_DEVICE_ERROR; }
            result_.store(result, std::memory_order_release);
            close_.wait(false);
            stopResult_ = device.Stop();
        });
    }

    bool Opening() const noexcept { return opening_; }
    bool OwnsDevice() const noexcept { return thread_.joinable(); }
    VnResult Finish(DeviceBackend& device) noexcept {
        const auto result = result_.load(std::memory_order_acquire);
        if (result != VN_OK) return result;
        if (!device.CommitRenderAdmission(generation_)) return VN_DEVICE_ERROR;
        opening_ = false;
        return VN_OK;
    }
    VnResult Stop() noexcept {
        if (thread_.joinable()) {
            close_.store(true);
            close_.notify_one();
            thread_.join();
        }
        opening_ = false;
        return stopResult_;
    }
private:
    std::thread thread_;
    std::atomic<bool> opening_{false};
    std::atomic<bool> close_{false};
    std::atomic<VnResult> result_{VN_IN_USE};
    std::uint64_t generation_{};
    VnResult stopResult_ = VN_OK;
};
}
