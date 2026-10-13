#pragma once

#include "MonitorProjectionTypes.h"

namespace VNX {
namespace Monitor {

MonitorGenerationProjection projectGeneration(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot);

} // namespace Monitor
} // namespace VNX
