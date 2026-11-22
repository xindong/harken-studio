#pragma once

#include "MonitorStore.h"

#include <cstdint>
#include <memory>

namespace harken::studio {
namespace Monitor {

bool entityReplayIsIncomplete(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot,
    std::uint32_t entityKind);

} // namespace Monitor
} // namespace harken::studio
