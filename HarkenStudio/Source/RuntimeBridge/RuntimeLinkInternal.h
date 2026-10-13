#pragma once

#include "PlaybackBridge.h"
#include <harken_preview/PreviewProject.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace HarkenEditor {
namespace Bridge {
namespace Detail {

enum class RuntimeProjectGenerationPrepareStatus : std::uint8_t {
    Ok = 0,
    InvalidView = 1,
    Backpressure = 2,
    Capacity = 3,
    CapabilityUnavailable = 4,
    DeviceError = 5,
    InvalidWorld = 6,
    RuntimeUnavailable = 7,
};

enum class RuntimeProjectGenerationCommitStatus : std::uint8_t {
    Ok = 0,
    Backpressure = 1,
    InvalidPrepared = 2,
    DeviceError = 3,
    RuntimeUnavailable = 4,
};

struct RuntimeLiveOwnerRevision {
    bool found = false;
    bool engineActive = false;
    std::uint64_t currentRevision = 0;
};

class RuntimePreparedProject {
public:
    RuntimePreparedProject() = default;
    RuntimePreparedProject(const RuntimePreparedProject&) = delete;
    RuntimePreparedProject& operator=(const RuntimePreparedProject&) = delete;
    RuntimePreparedProject(RuntimePreparedProject&& other) noexcept
        : handle(std::exchange(other.handle, 0)) {}
    RuntimePreparedProject& operator=(RuntimePreparedProject&& other) noexcept {
        if (this != &other) {
            VnPreview_CancelProject(handle);
            handle = std::exchange(other.handle, 0);
        }
        return *this;
    }
    ~RuntimePreparedProject() { VnPreview_CancelProject(handle); }
    bool Valid() const noexcept { return handle != 0; }
    VnPreviewProjectHandle handle = 0;
};

RuntimeLiveOwnerRevision runtimeLiveOwnerRevision(std::uint64_t ownerId);
RuntimeProjectGenerationPrepareStatus runtimePrepareProjectGeneration(
    const VnPreviewProjectDescV1& view,
    RuntimePreparedProject& out);
RuntimeProjectGenerationPrepareStatus runtimePrepareProjectGeneration(
    const VnPreviewProjectDescV2& view,
    RuntimePreparedProject& out);
RuntimeProjectGenerationPrepareStatus runtimePrepareProjectGeneration(
    const VnPreviewProjectDescV3& view,
    RuntimePreparedProject& out);
RuntimeProjectGenerationCommitStatus runtimeCommitPreparedProjectGeneration(
    RuntimePreparedProject& prepared,
    std::uint64_t* outInstalledWorldGeneration);
void runtimeCancelPreparedProjectGeneration(
    RuntimePreparedProject&& prepared);
std::uint32_t runtimeDrainProjectGenerationAcks();
void runtimePumpNullMixer(std::uint32_t blockCount);
void runtimeConfirmBusTopologyWithoutApply(
    const std::vector<RuntimeBusDefinition>& buses,
    const std::vector<RuntimeBusSendDefinition>& sends);

} // namespace Detail
} // namespace Bridge
} // namespace HarkenEditor
