#pragma once

#include "MonitorProjectionTypes.h"

namespace VNX {
namespace Monitor {

MonitorDropProjection projectDrop(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot,
    const MonitorSubscriptionProjection& subscription = {},
    uint64_t storeLocalSaturationCount = 0);

} // namespace Monitor
} // namespace VNX
