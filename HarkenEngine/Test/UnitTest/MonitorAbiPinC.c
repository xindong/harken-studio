#include <harken/HarkenDevelopment.h>

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
#error "MonitorAbiPinC.c must be compiled as C"
#endif

static const VnMonitorSessionHandle kUnusedHandle;
static const VnMonitorRegistryKind kUnusedRegistryKind;

enum {
    kMonitorPublicFunctionCount = 16
};

VnResult vn_monitor_abi_pin_c_probe(void) {
    VnMonitorCapabilities capabilities;
    capabilities.structSize = (uint32_t)sizeof(capabilities);
    (void)kUnusedHandle;
    (void)kUnusedRegistryKind;
    (void)kMonitorPublicFunctionCount;
    return Vn_MonitorGetCapabilities(&capabilities);
}
