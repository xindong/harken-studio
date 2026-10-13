#include "MonitorRecordingBrowser.h"
#include "Monitor/MonitorTypes.h"

namespace VNX {
namespace Monitor {
namespace {

bool hasCompleteFooter(const MonitorSessionFileResult& file)
{
    return file.status == kOk
        && file.commitsWritten != 0
        && file.droppedRequired == 0
        && file.bytesConsumed != 0;
}

} // namespace

MonitorRecordingBrowserState projectRecordingBrowser(
    const MonitorRecordingBrowserInput& input)
{
    MonitorRecordingBrowserState state;
    state.mode = input.mode;
    state.queryResult = input.statusQuery;
    state.engineState = input.status.state;
    state.fileBytes = input.status.fileBytes;
    state.lastCommittedChunk = input.status.lastCommittedChunk;
    state.queueBytes = input.status.queueBytes;
    state.recoveryNeeded = input.status.recoveryNeeded;
    state.errorReason = input.status.errorReason;
    state.commitsWritten = input.file.commitsWritten;
    state.hasCompleteFooter = input.fileLoaded && hasCompleteFooter(input.file);

    if (input.mode == MonitorRecordingMode::Remote)
    {
        state.face = MonitorRecordingFace::Unsupported;
        state.reason = "Remote recording unsupported";
        return state;
    }
    if (!input.contractPresent)
    {
        state.face = MonitorRecordingFace::Unsupported;
        state.reason = "Local recording contract absent";
        return state;
    }
    if (!input.hasRecordPermission || !canApply(input.captureControl))
    {
        state.face = MonitorRecordingFace::ReadOnly;
        state.reason = input.hasRecordPermission ? "CaptureControl denied"
                                                 : "Record denied";
        return state;
    }

    if (input.fileLoaded && !state.hasCompleteFooter)
    {
        state.face = MonitorRecordingFace::IncompleteFooter;
        state.reason = "No complete footer";
        state.startEnabled = true;
        return state;
    }

    switch (input.status.state)
    {
    case kMonitorRecordingStateActive:
        state.face = MonitorRecordingFace::Active;
        state.stopRecordingEnabled = true;
        break;
    case kMonitorRecordingStateStopping:
        state.face = MonitorRecordingFace::Stopping;
        state.reason = "Uncommitted chunk not recoverable";
        break;
    case kMonitorRecordingStateFailed:
        state.face = MonitorRecordingFace::Failed;
        state.startEnabled = true;
        break;
    default:
        state.face = MonitorRecordingFace::Stopped;
        state.startEnabled = true;
        break;
    }
    return state;
}

MonitorRecordingDispatch dispatchRecordingCommand(
    const MonitorRecordingBrowserState& state,
    MonitorRecordingCommand command)
{
    MonitorRecordingDispatch dispatch;
    dispatch.command = command;
    dispatch.runtimeEntry = recordingCommandEntry(command);
    if (command == MonitorRecordingCommand::None)
        return dispatch;

    if (state.mode == MonitorRecordingMode::Remote)
    {
        dispatch.reason = "Remote recording unsupported";
        return dispatch;
    }

    if (command == MonitorRecordingCommand::StartRecording)
    {
        if (!state.startEnabled)
        {
            dispatch.reason = state.reason.empty() ? "StartRecording denied"
                                                   : state.reason;
            return dispatch;
        }
        dispatch.accepted = true;
        return dispatch;
    }
    if (command == MonitorRecordingCommand::StopRecording)
    {
        if (!state.stopRecordingEnabled)
        {
            dispatch.reason = "StopRecording denied";
            return dispatch;
        }
        dispatch.accepted = true;
        return dispatch;
    }
    if (command == MonitorRecordingCommand::StopCapture
        || command == MonitorRecordingCommand::CloseSession)
    {
        dispatch.accepted = true;
        return dispatch;
    }
    dispatch.reason = "Unknown command";
    return dispatch;
}

const char* recordingFaceText(MonitorRecordingFace face)
{
    switch (face)
    {
    case MonitorRecordingFace::Unsupported:
        return "Unsupported";
    case MonitorRecordingFace::ReadOnly:
        return "ReadOnly";
    case MonitorRecordingFace::Stopped:
        return "Stopped";
    case MonitorRecordingFace::Active:
        return "Active";
    case MonitorRecordingFace::Stopping:
        return "Stopping";
    case MonitorRecordingFace::Failed:
        return "Failed";
    case MonitorRecordingFace::IncompleteFooter:
        return "IncompleteFooter";
    }
    return "Unsupported";
}

const char* recordingCommandEntry(MonitorRecordingCommand command)
{
    switch (command)
    {
    case MonitorRecordingCommand::StartRecording:
        return "Vn_MonitorStartRecording";
    case MonitorRecordingCommand::StopRecording:
        return "Vn_MonitorStopRecording";
    case MonitorRecordingCommand::StopCapture:
        return "Vn_MonitorStopCapture";
    case MonitorRecordingCommand::CloseSession:
        return "Vn_MonitorCloseSession";
    case MonitorRecordingCommand::None:
        return "";
    }
    return "";
}

} // namespace Monitor
} // namespace VNX
