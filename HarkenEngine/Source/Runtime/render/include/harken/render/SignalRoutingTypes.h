#ifndef HARKEN_RENDER_SIGNAL_ROUTING_TYPES_H
#define HARKEN_RENDER_SIGNAL_ROUTING_TYPES_H
#include <harken/Harken.h>
namespace harken {
struct SignalBusRoutingInfo {
    VnBusId id{};
    std::uint32_t generation = 0;
    double inputBound = 1;
    double outputFilterBound = 1;
    double outputGainBound = 1;
};
}
#endif
