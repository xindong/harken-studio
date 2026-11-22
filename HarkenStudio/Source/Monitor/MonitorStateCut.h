#pragma once

#include "MonitorProjectionTypes.h"

namespace harken::studio {
namespace Monitor {

MonitorStateCutProjection projectStateCut(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot);

} // namespace Monitor
} // namespace harken::studio
