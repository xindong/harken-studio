#pragma once

#include <functional>
#include "Monitor/MonitorTypes.h"
#include <string>

namespace VNX {
namespace Monitor {

struct MonitorPrincipal
{
    bool authenticated = false;
    std::string id;
};

// logicThreadPoster must run the work on the engine-owning logic thread
// before returning. Queue-and-return is not a valid poster: postControl
// treats a callback that did not run as kMonitorWrongThread and must
// not leave a delayed lambda holding stack references.
struct MonitorHostContext
{
    std::function<void(std::function<void()>)> logicThreadPoster;
    MonitorPrincipal principal;
};

} // namespace Monitor
} // namespace VNX
