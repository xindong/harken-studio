#pragma once

#include "MonitorStore.h"

#include <cstdint>
#include <memory>

namespace VNX {
namespace Monitor {

enum class TapSelectFace
{
    MissingRegistry = 0,
};

struct TapBindingProjection
{
    TapSelectFace face = TapSelectFace::MissingRegistry;
    const char* gapId = "P5-1";
    std::uint64_t generation = 0;
    bool registryPublished = false;
    bool bound = false;
};

bool tapRegistryPublished();
bool tapBindingPublished();
TapBindingProjection projectTapSelectFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot);

} // namespace Monitor
} // namespace VNX
