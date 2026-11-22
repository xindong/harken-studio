#pragma once

#include "MonitorProjectionTypes.h"

namespace harken::studio {
namespace Monitor {

MonitorGenerationProjection projectGeneration(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot);

} // namespace Monitor
} // namespace harken::studio
