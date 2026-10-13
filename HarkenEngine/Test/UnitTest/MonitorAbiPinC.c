#include <harken/Harken.h>

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
#error "MonitorAbiPinC.c must be compiled as C"
#endif

static const VnMonitorSessionHandle kUnusedHandle;
static const VnMonitorRegistryKind kUnusedRegistryKind;
static const VnMonitorChunkKind kUnusedChunkKind;

enum {
    kMonitorPublicFunctionCount = 20
};

VnResult vn_monitor_abi_pin_c_probe(void) {
    VnMonitorCapabilities capabilities;
    VnMonitorSessionCompareDesc compareDesc;
    VnMonitorCompareResult compareResult;
    capabilities.structSize = (uint32_t)sizeof(capabilities);
    compareDesc.structSize = (uint32_t)sizeof(compareDesc);
    compareDesc.reserved0 = 0;
    compareDesc.left = 0;
    compareDesc.right = 0;
    compareDesc.executeFrameBegin = 0;
    compareDesc.executeFrameEnd = 0;
    compareResult.structSize = (uint32_t)sizeof(compareResult);
    compareResult.result = 0xFFFFFFFFu;
    (void)kUnusedHandle;
    (void)kUnusedRegistryKind;
    (void)kUnusedChunkKind;
    (void)kMonitorPublicFunctionCount;
    if (Vn_MonitorCompareSessions(&compareDesc, &compareResult) != VN_INVALID_ARG) {
        return VN_INVALID_ARG;
    }
    if (compareResult.result != 0) {
        return VN_INVALID_ARG;
    }
    return Vn_MonitorGetCapabilities(&capabilities);
}
