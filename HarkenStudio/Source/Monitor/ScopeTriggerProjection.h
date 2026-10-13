#pragma once

#include "MonitorStore.h"

#include <cstdint>
#include <memory>

namespace VNX {
namespace Monitor {

enum class ScopeFace
{
    Unpublished = 0,
};

// Empty-state face only. snapshot is host generation plumbing.
// This pack never reads SCOPE_RESULT, triggerFrame, noTrigger, or window.
// windowPublished / triggerPublished stay false until a later pack
// publishes bytes. They are not a freeze / trigger state machine.
struct ScopeTriggerProjection
{
    ScopeFace face = ScopeFace::Unpublished;
    const char* gapId = "P5-3";
    std::uint64_t generation = 0;
    bool windowPublished = false;
    bool triggerPublished = false;
};

bool scopeWindowPublished();
bool scopeTriggerPublished();
ScopeTriggerProjection projectScopeFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot);

} // namespace Monitor
} // namespace VNX
