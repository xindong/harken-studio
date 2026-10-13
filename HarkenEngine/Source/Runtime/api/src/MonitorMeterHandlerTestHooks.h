#ifndef HARKEN_API_MONITOR_METER_HANDLER_TEST_HOOKS_H
#define HARKEN_API_MONITOR_METER_HANDLER_TEST_HOOKS_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include <cstdint>

namespace harken {
namespace monitor {
namespace meter_test {

void SetGainReductionLookupForTests(
    bool (*lookup)(const VnMonitorTapDescriptor& tap, float* reductionDb,
                   VnObjectId* ownerStableId,
                   std::uint64_t* ownerInstanceHandle,
                   std::uint64_t* ownerGeneration, void* ctx),
    void* ctx);

} // namespace meter_test
} // namespace monitor
} // namespace harken

#endif
