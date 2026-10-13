#pragma once

#include "MonitorCaptureControl.h"

#include "Monitor/MonitorTypes.h"

#include <cstdint>
#include <string>

namespace VNX {
namespace Monitor {

enum class MonitorRemoteFace
{
    Unsupported = 0,
    HandshakeRejected,
    ConnectedObserver,
    ConnectedOwner,
    Disconnecting,
    Disconnected,
};

struct MonitorRemotePermissionRow
{
    const char* id = "";
    uint32_t mask = 0;
    bool granted = false;
};

struct MonitorRemoteStatusState
{
    MonitorRemoteFace face = MonitorRemoteFace::Unsupported;
    MonitorResult queryResult = kNotInitialized;
    uint32_t securityProfile = 0;
    uint32_t permissionMask = 0;
    uint64_t principalHash = 0;
    uint64_t messageCounter = 0;
    uint64_t reconnectGeneration = 0;
    uint64_t byteRate = 0;
    uint64_t queueBytes = 0;
    uint32_t dropCount = 0;
    uint64_t leaseGeneration = 0;
    MonitorCaptureControlState captureControl {};
    MonitorRemotePermissionRow permissions[7] {};
};

struct MonitorRemoteStatusInput
{
    MonitorResult queryResult = kNotInitialized;
    bool contractPresent = false;
    bool handshakeCompatible = true;
    uint32_t securityProfile = 0;
    uint32_t permissionMask = 0;
    uint64_t principalHash = 0;
    uint64_t messageCounter = 0;
    uint64_t reconnectGeneration = 0;
    uint64_t byteRate = 0;
    uint64_t queueBytes = 0;
    uint32_t dropCount = 0;
    bool disconnecting = false;
    bool disconnected = false;
    bool sequenceExhausted = false;
    MonitorCaptureControlState captureControl {};
};

MonitorRemoteStatusState projectRemoteStatus(const MonitorRemoteStatusInput& input);

bool permissionGranted(const MonitorRemoteStatusState& state, uint32_t mask);
bool connectedHasControl(const MonitorRemoteStatusState& state);
bool observationOnly(const MonitorRemoteStatusState& state);

std::string remoteStatusFaceText(const MonitorRemoteStatusState& state);
std::string permissionDeniedReason(uint32_t mask);
std::string recordDeniedReason(const MonitorRemoteStatusState& state);

} // namespace Monitor
} // namespace VNX
