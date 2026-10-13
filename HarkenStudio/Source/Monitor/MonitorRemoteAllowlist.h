#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace VNX {
namespace Monitor {

struct MonitorRemoteAllowlistEntry
{
    const char* name = "";
    bool observeOnly = false;
};

const MonitorRemoteAllowlistEntry* monitorRemoteObserveAllowlist(std::size_t* count);

bool isMonitorRemoteObserveName(const char* name);
bool isMonitorRemoteMutationName(const char* name);
bool allowlistRejectsMutation(const char* name);

std::vector<std::string> productionObserveAllowlistNames();

} // namespace Monitor
} // namespace VNX
