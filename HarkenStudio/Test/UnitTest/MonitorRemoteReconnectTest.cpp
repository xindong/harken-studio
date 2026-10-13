#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorAdapter.h"
#include "Monitor/MonitorCaptureControlHost.h"
#include "Monitor/MonitorRemoteStatus.h"
#include "Widgets/Monitor/MonitorRemoteStatusStrip.h"
#include "Widgets/TimeSeries/MonitorCursorModel.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QApplication>

#include <fstream>
#include <iterator>
#include <string>

using VNX::MonitorCursorModel;
using VNX::Monitor::MonitorAdapter;
using VNX::Monitor::MonitorCaptureControlHost;
using VNX::Monitor::MonitorHostContext;
using VNX::Monitor::MonitorRemoteFace;
using VNX::Monitor::MonitorRemoteStatusInput;
using VNX::Monitor::connectedHasControl;
using VNX::Monitor::projectRemoteStatus;
using VNX::Monitor::recordDeniedReason;
using VNX::Monitor::remoteStatusFaceText;

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
MonitorHostContext syncHost()
{
    MonitorHostContext host;
    host.principal.authenticated = true;
    host.principal.id = "studio-local";
    host.logicThreadPoster = [](std::function<void()> work) {
        if (work)
            work();
    };
    return host;
}

VNX::Monitor::MonitorSessionDesc validDesc()
{
    VNX::Monitor::MonitorSessionDesc desc {};
    return desc;
}

bool tracesContain(const MonitorAdapter& adapter, const char* entry)
{
    for (const auto& trace : adapter.callTrace())
    {
        if (trace.entry != nullptr && std::string(trace.entry) == entry)
            return true;
    }
    return false;
}

} // namespace

TEST(MonitorRemoteReconnect, DisconnectingFaceIsNotConnected)
{
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.permissionMask = VN_MONITOR_PERM_CAPTURE_CONTROL | VN_MONITOR_PERM_RECORD;
    input.disconnecting = true;
    const auto state = projectRemoteStatus(input);
    EXPECT_EQ(state.face, MonitorRemoteFace::Disconnecting);
    EXPECT_FALSE(connectedHasControl(state));
    EXPECT_EQ(recordDeniedReason(state), "Disconnecting");
}

TEST(MonitorRemoteReconnect, DisconnectedFaceIsNotConnected)
{
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.permissionMask = VN_MONITOR_PERM_CAPTURE_CONTROL | VN_MONITOR_PERM_RECORD;
    input.disconnected = true;
    const auto state = projectRemoteStatus(input);
    EXPECT_EQ(state.face, MonitorRemoteFace::Disconnected);
    EXPECT_FALSE(connectedHasControl(state));
    EXPECT_EQ(remoteStatusFaceText(state), "Disconnected");
}

TEST(MonitorRemoteReconnect, SequenceExhaustedIsDisconnected)
{
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.sequenceExhausted = true;
    const auto state = projectRemoteStatus(input);
    EXPECT_EQ(state.face, MonitorRemoteFace::Disconnected);
    EXPECT_FALSE(connectedHasControl(state));
}

TEST(MonitorRemoteReconnect, StripDisconnectingDisablesControl)
{
    ensureApp();
    VNX::MonitorRemoteStatusStrip strip;
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.permissionMask = VN_MONITOR_PERM_CAPTURE_CONTROL | VN_MONITOR_PERM_RECORD;
    input.disconnecting = true;
    strip.applyState(projectRemoteStatus(input));
    EXPECT_FALSE(strip.controlEnabled());
    EXPECT_FALSE(strip.recordEnabled());
}

TEST(MonitorRemoteReconnect, StaleEpochDoesNotTraceMutation)
{
    VnConfig config {};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);

    MonitorAdapter adapter(syncHost());
    auto desc = validDesc();
    const auto opened = adapter.open(&desc);
    ASSERT_EQ(opened.result, VN_OK);
    const uint64_t oldEpoch = opened.epoch;
    const auto handle = opened.handle;
    ASSERT_EQ(adapter.close(handle), VN_OK);
    const auto reopened = adapter.open(&desc);
    ASSERT_EQ(reopened.result, VN_OK);
    ASSERT_NE(reopened.epoch, oldEpoch);

    VNX::Monitor::MonitorCaptureControlStatus status {};
    EXPECT_EQ(adapter.acquireCaptureControl(handle, &status, oldEpoch),
              VN_MONITOR_STALE_SESSION);
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorAcquireCaptureControl"));

    VNX::Monitor::MonitorCapturePlan plan {};
    VNX::Monitor::MonitorEffectiveCapturePlan accepted {};
    EXPECT_EQ(adapter.applyCapturePlan(handle, 1, &plan, &accepted, oldEpoch),
              VN_MONITOR_STALE_SESSION);
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorApplyCapturePlan"));

    VNX::Monitor::MonitorRecordingDesc recording {};
    recording.utf8Path = "stale.vnmon";
    EXPECT_EQ(adapter.startRecording(handle, 1, &recording, oldEpoch),
              VN_MONITOR_STALE_SESSION);
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorStartRecording"));
    EXPECT_EQ(adapter.stopRecording(handle, 1, oldEpoch), VN_MONITOR_STALE_SESSION);
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorStopRecording"));

    EXPECT_EQ(adapter.close(reopened.handle), VN_OK);
    Vn_Term();
}

TEST(MonitorRemoteReconnect, LocalSessionLossDoesNotClaimRemoteDisconnect)
{
    VnConfig config {};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);

    MonitorAdapter adapter(syncHost());
    MonitorCaptureControlHost host(&adapter);
    host.openLayout();
    ASSERT_TRUE(adapter.hasOpenSession());
    ASSERT_EQ(adapter.close(adapter.sessionHandle()), VN_OK);
    ASSERT_FALSE(adapter.hasOpenSession());

    host.openLayout();
    EXPECT_EQ(host.lastRemoteState().face, MonitorRemoteFace::Unsupported);
    EXPECT_FALSE(connectedHasControl(host.lastRemoteState()));
    EXPECT_FALSE(adapter.hasOpenSession());
    EXPECT_FALSE(host.tracesContain("Vn_MonitorStopRecording"));

    host.openLayout();
    EXPECT_TRUE(adapter.hasOpenSession());
    EXPECT_NE(host.lastRemoteState().face, MonitorRemoteFace::Disconnected);
    EXPECT_NE(host.lastRemoteState().face, MonitorRemoteFace::Disconnecting);

    ASSERT_EQ(host.reconnect(), VN_OK);
    EXPECT_TRUE(adapter.hasOpenSession());
    EXPECT_NE(host.lastRemoteState().face, MonitorRemoteFace::Disconnected);
    EXPECT_NE(host.lastRemoteState().face, MonitorRemoteFace::Disconnecting);

    adapter.close(adapter.sessionHandle());
    Vn_Term();
}

TEST(MonitorRemoteReconnect, HostReconnectOpensNewEpochWithoutStopRecording)
{
    VnConfig config {};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);

    MonitorAdapter adapter(syncHost());
    MonitorCaptureControlHost host(&adapter);
    host.openLayout();
    ASSERT_TRUE(adapter.hasOpenSession());
    const uint64_t firstEpoch = adapter.sessionEpoch();
    host.markDisconnecting();
    EXPECT_EQ(host.lastRemoteState().face, MonitorRemoteFace::Unsupported);
    EXPECT_FALSE(connectedHasControl(host.lastRemoteState()));
    host.markDisconnected();
    EXPECT_EQ(host.lastRemoteState().face, MonitorRemoteFace::Unsupported);
    EXPECT_FALSE(adapter.hasOpenSession());
    EXPECT_FALSE(host.tracesContain("Vn_MonitorStopRecording"));

    ASSERT_EQ(host.reconnect(), VN_OK);
    ASSERT_TRUE(adapter.hasOpenSession());
    EXPECT_NE(adapter.sessionEpoch(), firstEpoch);
    EXPECT_NE(host.lastRemoteState().face, MonitorRemoteFace::Disconnecting);
    EXPECT_NE(host.lastRemoteState().face, MonitorRemoteFace::Disconnected);
    EXPECT_FALSE(host.tracesContain("Vn_MonitorStopRecording"));

    adapter.close(adapter.sessionHandle());
    Vn_Term();
}

TEST(MonitorRemoteReconnect, MainWindowRefreshProjectsDisconnectWithoutStopRecording)
{
    const std::string path = std::string(HARKEN_STUDIO_SOURCE_DIR) + "/Source/MainWindow.cpp";
    std::ifstream in(path);
    ASSERT_TRUE(in) << path;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("monitorCaptureControlHost_->openLayout()"), std::string::npos);
    EXPECT_NE(text.find("applyRemoteStatusState(monitorCaptureControlHost_->lastRemoteState())"),
              std::string::npos);
    EXPECT_EQ(text.find("monitorCaptureControlHost_->reconnect()"), std::string::npos);
    EXPECT_EQ(text.find("Vn_MonitorStopRecording"), std::string::npos);
}

TEST(MonitorRemoteReconnect, CursorDropsOldFrameOnNewEpoch)
{
    MonitorCursorModel model;
    model.setSessionIdentity(1, 1);
    ASSERT_TRUE(model.advanceLive(40));
    ASSERT_TRUE(model.snapshot().executeFrame.has_value());
    EXPECT_EQ(*model.snapshot().executeFrame, 40);

    model.setSessionIdentity(2, 1);
    EXPECT_EQ(model.snapshot().sessionEpoch, 2u);
    EXPECT_FALSE(model.snapshot().executeFrame.has_value());
}
