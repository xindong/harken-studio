#pragma once

#include "MonitorProjectionTypes.h"

namespace VNX {
namespace Monitor {

MonitorStateCutProjection projectStateCut(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot);

} // namespace Monitor
} // namespace VNX
