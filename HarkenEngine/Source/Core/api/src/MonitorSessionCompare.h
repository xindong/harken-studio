#ifndef HARKEN_API_MONITOR_SESSION_COMPARE_H
#define HARKEN_API_MONITOR_SESSION_COMPARE_H

#include <harken/Harken.h>

namespace harken {
namespace monitor {

VnResult CompareSessions(const VnMonitorSessionCompareDesc* desc,
                         VnMonitorCompareResult* out);

} // namespace monitor
} // namespace harken

#endif
