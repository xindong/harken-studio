#pragma once

#include "MonitorCaptureControl.h"
#include "MonitorSessionFile.h"

#include "Monitor/MonitorTypes.h"

#include <cstdint>
#include <string>

namespace VNX {
namespace Monitor {

enum class MonitorRecordingMode
{
    Local = 0,
    Remote,
};

enum class MonitorRecordingFace
{
    Unsupported = 0,
    ReadOnly,
    Stopped,
    Active,
    Stopping,
    Failed,
    IncompleteFooter,
};

enum class MonitorRecordingCommand
{
    None = 0,
    StartRecording,
    StopRecording,
    StopCapture,
    CloseSession,
};

struct MonitorRecordingBrowserState
{
    MonitorRecordingFace face = MonitorRecordingFace::Unsupported;
    MonitorRecordingMode mode = MonitorRecordingMode::Local;
    MonitorResult queryResult = kNotInitialized;
    uint32_t engineState = kMonitorRecordingStateStopped;
    uint64_t fileBytes = 0;
    uint64_t lastCommittedChunk = 0;
    uint64_t queueBytes = 0;
    uint32_t recoveryNeeded = 0;
    uint32_t errorReason = 0;
    uint32_t commitsWritten = 0;
    bool hasCompleteFooter = false;
    bool startEnabled = false;
    bool stopRecordingEnabled = false;
    std::string reason;
};

struct MonitorRecordingBrowserInput
{
    MonitorRecordingMode mode = MonitorRecordingMode::Local;
    bool contractPresent = false;
    bool hasRecordPermission = false;
    MonitorCaptureControlState captureControl {};
    MonitorResult statusQuery = kNotInitialized;
    MonitorRecordingStatus status {};
    MonitorSessionFileResult file {};
    bool fileLoaded = false;
};

struct MonitorRecordingDispatch
{
    MonitorRecordingCommand command = MonitorRecordingCommand::None;
    bool accepted = false;
    const char* runtimeEntry = "";
    std::string reason;
};

MonitorRecordingBrowserState projectRecordingBrowser(
    const MonitorRecordingBrowserInput& input);

MonitorRecordingDispatch dispatchRecordingCommand(
    const MonitorRecordingBrowserState& state,
    MonitorRecordingCommand command);

const char* recordingFaceText(MonitorRecordingFace face);
const char* recordingCommandEntry(MonitorRecordingCommand command);

} // namespace Monitor
} // namespace VNX
