#include "MonitorRemoteStatus.h"
#include "Monitor/MonitorTypes.h"

namespace VNX {
namespace Monitor {
namespace {

const uint32_t kPermissionMasks[7] = {
    kMonitorPermObserveState,
    kMonitorPermObserveEvents,
    kMonitorPermObserveMeasurements,
    kMonitorPermCaptureControl,
    kMonitorPermRecord,
    kMonitorPermRawAudio,
    kMonitorPermSymbols,
};

const char* kPermissionIds[7] = {
    "ObserveState",
    "ObserveEvents",
    "ObserveMeasurements",
    "CaptureControl",
    "Record",
    "RawAudio",
    "Symbols",
};

} // namespace

MonitorRemoteStatusState projectRemoteStatus(const MonitorRemoteStatusInput& input)
{
    MonitorRemoteStatusState state;
    state.queryResult = input.queryResult;
    state.securityProfile = input.securityProfile;
    state.permissionMask = input.permissionMask;
    state.principalHash = input.principalHash;
    state.messageCounter = input.messageCounter;
    state.reconnectGeneration = input.reconnectGeneration;
    state.byteRate = input.byteRate;
    state.queueBytes = input.queueBytes;
    state.dropCount = input.dropCount;
    state.leaseGeneration = input.captureControl.leaseGeneration;
    state.captureControl = input.captureControl;

    for (int i = 0; i < 7; ++i)
    {
        state.permissions[i].id = kPermissionIds[i];
        state.permissions[i].mask = kPermissionMasks[i];
        state.permissions[i].granted = false;
    }

    if (!input.handshakeCompatible)
    {
        state.face = MonitorRemoteFace::HandshakeRejected;
        return state;
    }
    if (input.sequenceExhausted)
    {
        state.face = MonitorRemoteFace::Disconnected;
        return state;
    }
    if (input.disconnecting)
    {
        state.face = MonitorRemoteFace::Disconnecting;
        return state;
    }
    if (input.disconnected)
    {
        state.face = MonitorRemoteFace::Disconnected;
        return state;
    }
    if (!input.contractPresent || input.queryResult != kOk)
    {
        state.face = MonitorRemoteFace::Unsupported;
        return state;
    }

    for (int i = 0; i < 7; ++i)
        state.permissions[i].granted =
            (input.permissionMask & kPermissionMasks[i]) == kPermissionMasks[i];

    const bool hasCaptureControl =
        permissionGranted(state, kMonitorPermCaptureControl);
    if (hasCaptureControl
        && canApply(input.captureControl)
        && input.captureControl.leaseGeneration == input.reconnectGeneration)
        state.face = MonitorRemoteFace::ConnectedOwner;
    else
        state.face = MonitorRemoteFace::ConnectedObserver;
    return state;
}

bool permissionGranted(const MonitorRemoteStatusState& state, uint32_t mask)
{
    for (const auto& row : state.permissions)
    {
        if (row.mask == mask)
            return row.granted;
    }
    return false;
}

bool connectedHasControl(const MonitorRemoteStatusState& state)
{
    return state.face == MonitorRemoteFace::ConnectedOwner;
}

bool observationOnly(const MonitorRemoteStatusState& state)
{
    return state.face == MonitorRemoteFace::ConnectedObserver
        || state.face == MonitorRemoteFace::ConnectedOwner;
}

std::string remoteStatusFaceText(const MonitorRemoteStatusState& state)
{
    switch (state.face)
    {
    case MonitorRemoteFace::Unsupported:
        return "Unsupported";
    case MonitorRemoteFace::HandshakeRejected:
        return "HandshakeRejected";
    case MonitorRemoteFace::ConnectedObserver:
        return "ConnectedObserver";
    case MonitorRemoteFace::ConnectedOwner:
        return "ConnectedOwner";
    case MonitorRemoteFace::Disconnecting:
        return "Disconnecting";
    case MonitorRemoteFace::Disconnected:
        return "Disconnected";
    }
    return "Unsupported";
}

std::string permissionDeniedReason(uint32_t mask)
{
    switch (mask)
    {
    case kMonitorPermObserveState:
        return "ObserveState denied";
    case kMonitorPermObserveEvents:
        return "ObserveEvents denied";
    case kMonitorPermObserveMeasurements:
        return "ObserveMeasurements denied";
    case kMonitorPermCaptureControl:
        return "CaptureControl denied";
    case kMonitorPermRecord:
        return "Record denied";
    case kMonitorPermRawAudio:
        return "RawAudio denied";
    case kMonitorPermSymbols:
        return "Symbols denied";
    default:
        return "Permission denied";
    }
}

std::string recordDeniedReason(const MonitorRemoteStatusState& state)
{
    if (permissionGranted(state, kMonitorPermRecord) && connectedHasControl(state))
        return {};
    if (state.face == MonitorRemoteFace::Unsupported)
        return "Unsupported";
    if (state.face == MonitorRemoteFace::HandshakeRejected)
        return "HandshakeRejected";
    if (state.face == MonitorRemoteFace::Disconnecting)
        return "Disconnecting";
    if (state.face == MonitorRemoteFace::Disconnected)
        return "Disconnected";
    if (!permissionGranted(state, kMonitorPermRecord))
        return permissionDeniedReason(kMonitorPermRecord);
    if (!permissionGranted(state, kMonitorPermCaptureControl))
        return permissionDeniedReason(kMonitorPermCaptureControl);
    return "StaleLease";
}

} // namespace Monitor
} // namespace VNX
