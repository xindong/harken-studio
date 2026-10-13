#include "MonitorRemoteAllowlist.h"

#include <cstring>

namespace VNX {
namespace Monitor {
namespace {

const MonitorRemoteAllowlistEntry kObserveAllowlist[] = {
    {"Vn_MonitorGetCapabilities", true},
    {"Vn_MonitorGetSubscriptionStatus", true},
    {"Vn_MonitorGetStateCutStatus", true},
    {"Vn_MonitorGetCaptureControlStatus", true},
    {"Vn_MonitorGetEffectiveCapturePlan", true},
    {"Vn_MonitorGetConsumerStatus", true},
    {"Vn_MonitorGetRecordingStatus", true},
    {"Vn_MonitorCaptureRegistry", true},
    {"Vn_MonitorPoll", true},
};

const char* kMutationNames[] = {
    "Vn_MonitorAcquireCaptureControl",
    "Vn_MonitorReleaseCaptureControl",
    "Vn_MonitorValidateCapturePlan",
    "Vn_MonitorApplyCapturePlan",
    "Vn_MonitorStopCapture",
    "Vn_MonitorStartRecording",
    "Vn_MonitorStopRecording",
    "Vn_MonitorOpenSession",
    "Vn_MonitorCloseSession",
    "Vn_MonitorRequestStateCut",
};

} // namespace

const MonitorRemoteAllowlistEntry* monitorRemoteObserveAllowlist(std::size_t* count)
{
    if (count != nullptr)
        *count = sizeof(kObserveAllowlist) / sizeof(kObserveAllowlist[0]);
    return kObserveAllowlist;
}

bool isMonitorRemoteObserveName(const char* name)
{
    if (name == nullptr)
        return false;
    std::size_t count = 0;
    const auto* entries = monitorRemoteObserveAllowlist(&count);
    for (std::size_t i = 0; i < count; ++i)
    {
        if (std::strcmp(entries[i].name, name) == 0)
            return true;
    }
    return false;
}

bool isMonitorRemoteMutationName(const char* name)
{
    if (name == nullptr)
        return false;
    for (const char* mutation : kMutationNames)
    {
        if (std::strcmp(mutation, name) == 0)
            return true;
    }
    return false;
}

bool allowlistRejectsMutation(const char* name)
{
    return isMonitorRemoteMutationName(name) && !isMonitorRemoteObserveName(name);
}

std::vector<std::string> productionObserveAllowlistNames()
{
    std::vector<std::string> names;
    std::size_t count = 0;
    const auto* entries = monitorRemoteObserveAllowlist(&count);
    names.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        names.emplace_back(entries[i].name);
    return names;
}

} // namespace Monitor
} // namespace VNX
