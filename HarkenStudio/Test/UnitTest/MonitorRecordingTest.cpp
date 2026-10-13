#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorCaptureControl.h"
#include "Monitor/MonitorRecordingBrowser.h"
#include "Widgets/Monitor/MonitorRecordingPanel.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QPushButton>

#include <fstream>
#include <iterator>
#include <string>

using VNX::Monitor::MonitorCaptureControlState;
using VNX::Monitor::MonitorLeaseProjection;
using VNX::Monitor::MonitorRecordingBrowserInput;
using VNX::Monitor::MonitorRecordingCommand;
using VNX::Monitor::MonitorRecordingFace;
using VNX::Monitor::MonitorRecordingMode;
using VNX::Monitor::MonitorSessionFileResult;
using VNX::Monitor::dispatchRecordingCommand;
using VNX::Monitor::projectCaptureControlStatus;
using VNX::Monitor::projectRecordingBrowser;
using VNX::Monitor::recordingCommandEntry;

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}
MonitorCaptureControlState ownerLease()
{
    VNX::Monitor::MonitorCaptureControlStatus status {};
    status.held = 1;
    status.leaseGeneration = 4;
    status.principalHash = 11;
    return projectCaptureControlStatus(VN_OK, &status, 11, 4);
}

MonitorRecordingBrowserInput localReady()
{
    MonitorRecordingBrowserInput input;
    input.mode = MonitorRecordingMode::Local;
    input.contractPresent = true;
    input.hasRecordPermission = true;
    input.captureControl = ownerLease();
    input.statusQuery = VN_OK;
    input.status.state = VN_MONITOR_RECORDING_STATE_STOPPED;
    return input;
}

} // namespace

TEST(MonitorRecording, RemoteIsUnsupportedAndStartRejected)
{
    MonitorRecordingBrowserInput input = localReady();
    input.mode = MonitorRecordingMode::Remote;
    const auto state = projectRecordingBrowser(input);
    EXPECT_EQ(state.face, MonitorRecordingFace::Unsupported);
    EXPECT_FALSE(state.startEnabled);
    const auto dispatched = dispatchRecordingCommand(
        state, MonitorRecordingCommand::StartRecording);
    EXPECT_FALSE(dispatched.accepted);
    EXPECT_STREQ(dispatched.runtimeEntry, "Vn_MonitorStartRecording");
}

TEST(MonitorRecording, LocalStoppedAllowsStart)
{
    const auto state = projectRecordingBrowser(localReady());
    EXPECT_EQ(state.face, MonitorRecordingFace::Stopped);
    EXPECT_TRUE(state.startEnabled);
    const auto dispatched = dispatchRecordingCommand(
        state, MonitorRecordingCommand::StartRecording);
    EXPECT_TRUE(dispatched.accepted);
}

TEST(MonitorRecording, MissingRecordPermissionIsReadOnly)
{
    auto input = localReady();
    input.hasRecordPermission = false;
    const auto state = projectRecordingBrowser(input);
    EXPECT_EQ(state.face, MonitorRecordingFace::ReadOnly);
    EXPECT_FALSE(state.startEnabled);
    EXPECT_FALSE(dispatchRecordingCommand(
                     state, MonitorRecordingCommand::StartRecording)
                     .accepted);
}

TEST(MonitorRecording, IncompleteFooterIsNotStoppedRecoverable)
{
    auto input = localReady();
    input.fileLoaded = true;
    input.file.status = VN_OK;
    input.file.commitsWritten = 0;
    input.file.bytesConsumed = 32;
    const auto state = projectRecordingBrowser(input);
    EXPECT_EQ(state.face, MonitorRecordingFace::IncompleteFooter);
    EXPECT_FALSE(state.hasCompleteFooter);
}

TEST(MonitorRecording, StoppingDoesNotLookRecoverable)
{
    auto input = localReady();
    input.status.state = VN_MONITOR_RECORDING_STATE_STOPPING;
    const auto state = projectRecordingBrowser(input);
    EXPECT_EQ(state.face, MonitorRecordingFace::Stopping);
    EXPECT_FALSE(state.startEnabled);
    EXPECT_FALSE(state.stopRecordingEnabled);
}

TEST(MonitorRecording, ThreeStopCommandsStayIsolated)
{
    auto input = localReady();
    input.status.state = VN_MONITOR_RECORDING_STATE_ACTIVE;
    const auto state = projectRecordingBrowser(input);
    const auto stopRecording = dispatchRecordingCommand(
        state, MonitorRecordingCommand::StopRecording);
    const auto stopCapture = dispatchRecordingCommand(
        state, MonitorRecordingCommand::StopCapture);
    const auto closeSession = dispatchRecordingCommand(
        state, MonitorRecordingCommand::CloseSession);
    EXPECT_TRUE(stopRecording.accepted);
    EXPECT_TRUE(stopCapture.accepted);
    EXPECT_TRUE(closeSession.accepted);
    EXPECT_STREQ(stopRecording.runtimeEntry, "Vn_MonitorStopRecording");
    EXPECT_STREQ(stopCapture.runtimeEntry, "Vn_MonitorStopCapture");
    EXPECT_STREQ(closeSession.runtimeEntry, "Vn_MonitorCloseSession");
    EXPECT_STRNE(recordingCommandEntry(MonitorRecordingCommand::StopRecording),
                 recordingCommandEntry(MonitorRecordingCommand::StopCapture));
    EXPECT_STRNE(recordingCommandEntry(MonitorRecordingCommand::StopRecording),
                 recordingCommandEntry(MonitorRecordingCommand::CloseSession));
}

TEST(MonitorRecording, PanelRemoteClickDoesNotStart)
{
    ensureApp();
    VNX::MonitorRecordingPanel panel;
    auto input = localReady();
    input.mode = MonitorRecordingMode::Remote;
    panel.applyState(projectRecordingBrowser(input));
    EXPECT_FALSE(panel.startEnabled());
    auto* start = panel.findChild<QPushButton*>(QStringLiteral("MonitorRecordingStart"));
    ASSERT_NE(start, nullptr);
    start->click();
    EXPECT_EQ(panel.startClicks(), 0);
}

TEST(MonitorRecording, PanelStopButtonsDoNotShareHandler)
{
    ensureApp();
    VNX::MonitorRecordingPanel panel;
    auto input = localReady();
    input.status.state = VN_MONITOR_RECORDING_STATE_ACTIVE;
    panel.applyState(projectRecordingBrowser(input));
    auto* stopRecording =
        panel.findChild<QPushButton*>(QStringLiteral("MonitorRecordingStop"));
    auto* stopCapture =
        panel.findChild<QPushButton*>(QStringLiteral("MonitorRecordingStopCapture"));
    auto* closeButton =
        panel.findChild<QPushButton*>(QStringLiteral("MonitorRecordingClose"));
    ASSERT_NE(stopRecording, nullptr);
    ASSERT_NE(stopCapture, nullptr);
    ASSERT_NE(closeButton, nullptr);
    stopRecording->click();
    EXPECT_EQ(panel.stopRecordingClicks(), 1);
    EXPECT_EQ(panel.stopCaptureClicks(), 0);
    EXPECT_EQ(panel.closeClicks(), 0);
    EXPECT_EQ(panel.lastAccepted(), MonitorRecordingCommand::StopRecording);
    stopCapture->click();
    EXPECT_EQ(panel.stopCaptureClicks(), 1);
    EXPECT_EQ(panel.lastAccepted(), MonitorRecordingCommand::StopCapture);
    closeButton->click();
    EXPECT_EQ(panel.closeClicks(), 1);
    EXPECT_EQ(panel.lastAccepted(), MonitorRecordingCommand::CloseSession);
}

TEST(MonitorRecording, PanelEmitsAcceptedCommandForHost)
{
    ensureApp();
    VNX::MonitorRecordingPanel panel;
    auto input = localReady();
    input.status.state = VN_MONITOR_RECORDING_STATE_ACTIVE;
    panel.applyState(projectRecordingBrowser(input));
    int received = 0;
    MonitorRecordingCommand last = MonitorRecordingCommand::None;
    QObject::connect(&panel, &VNX::MonitorRecordingPanel::recordingCommandRequested,
                     [&](MonitorRecordingCommand command) {
                         ++received;
                         last = command;
                     });
    auto* stopRecording =
        panel.findChild<QPushButton*>(QStringLiteral("MonitorRecordingStop"));
    ASSERT_NE(stopRecording, nullptr);
    stopRecording->click();
    EXPECT_EQ(received, 1);
    EXPECT_EQ(last, MonitorRecordingCommand::StopRecording);
    EXPECT_STREQ(recordingCommandEntry(last), "Vn_MonitorStopRecording");
}

TEST(MonitorRecording, HostWiresRecordingPanelType)
{
    const std::string keysPath = std::string(HARKEN_STUDIO_SOURCE_DIR)
        + "/Source/Widgets/Monitor/MonitorPanelKeys.cpp";
    const std::string mainPath = std::string(HARKEN_STUDIO_SOURCE_DIR) + "/Source/MainWindow.cpp";
    std::ifstream keys(keysPath);
    std::ifstream main(mainPath);
    ASSERT_TRUE(keys) << keysPath;
    ASSERT_TRUE(main) << mainPath;
    const std::string keysText((std::istreambuf_iterator<char>(keys)),
                               std::istreambuf_iterator<char>());
    const std::string mainText((std::istreambuf_iterator<char>(main)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(keysText.find("MonitorRecording"), std::string::npos);
    EXPECT_NE(mainText.find("registerSingletonFactory(QStringLiteral(\"MonitorRecording\")"),
              std::string::npos);
    EXPECT_EQ(mainText.find("Vn_MonitorStartRecording"), std::string::npos);
    EXPECT_EQ(mainText.find("Vn_MonitorStopRecording"), std::string::npos);
}
