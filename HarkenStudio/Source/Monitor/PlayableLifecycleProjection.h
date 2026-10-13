#pragma once

#include "MonitorStore.h"

#include "Monitor/MonitorTypes.h"

#include <cstdint>
#include <memory>

namespace VNX {
namespace Monitor {

enum class PlayableLifecycleCell
{
    UnsupportedAsOf = 0,
};

struct PlayableLifecycleView
{
    PlayableLifecycleCell cell = PlayableLifecycleCell::UnsupportedAsOf;
    const char* gapId = "P4-1";
};

enum class MonitorPlayablesFace
{
    UnsupportedAsOf = 0,
};

struct MonitorPlayablesProjection
{
    MonitorPlayablesFace face = MonitorPlayablesFace::UnsupportedAsOf;
    const char* gapId = "P4-1";
    std::uint64_t generation = 0;
    bool variableControlKindPublished = false;
};

bool playableAsOfStatePublished();
bool variableControlEntityKindPublished();
PlayableLifecycleView projectPlayableLifecycle(const MonitorRecordHeader& header);
MonitorPlayablesProjection projectPlayablesFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot);

} // namespace Monitor
} // namespace VNX
