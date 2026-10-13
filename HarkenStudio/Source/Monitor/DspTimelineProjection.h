#pragma once

#include "MonitorStore.h"

#include <cstdint>
#include <memory>

namespace VNX {
namespace Monitor {

enum class DspTimelineFace
{
    UnsupportedG19 = 0,
};

struct DspTimelineProjection
{
    DspTimelineFace face = DspTimelineFace::UnsupportedG19;
    const char* gapId = "G19";
    std::uint64_t generation = 0;
    bool wallPublished = false;
    bool workPublished = false;
    bool criticalPathPublished = false;
};

bool dspWallPublished();
bool dspWorkPublished();
bool dspCriticalPathPublished();
DspTimelineProjection projectDspTimelineFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot);

} // namespace Monitor
} // namespace VNX
