#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorAdapter.h"
#include "Monitor/MonitorCaptureControl.h"
#include "Monitor/MonitorCaptureControlHost.h"
#include "Monitor/MonitorHostContext.h"

#include "Core/BuiltinLayouts.h"
#include "Widgets/Monitor/MonitorCaptureControlStrip.h"
#include "Widgets/Monitor/MonitorPanelKeys.h"
#include "Widgets/Monitor/MonitorPerformancePanel.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QApplication>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using VNX::Monitor::MonitorAdapter;
using VNX::Monitor::MonitorCaptureControlHost;
using VNX::Monitor::MonitorCaptureControlState;
using VNX::Monitor::MonitorHostContext;
using VNX::Monitor::MonitorLeaseProjection;
using VNX::Monitor::MonitorMeasurementIntent;
using VNX::Monitor::canApply;
using VNX::Monitor::captureControlStatusText;
using VNX::Monitor::hidePanelMeasurement;
using VNX::Monitor::intentAllowed;
using VNX::Monitor::projectCaptureControlStatus;
using VNX::Monitor::zeroCaptureControlStatus;

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

} // namespace

TEST(MonitorCaptureControl, ZeroClearsCallerStatus)
{
    VNX::Monitor::MonitorCaptureControlStatus status {};
    status.held = 1;
    status.leaseGeneration = 8;
    status.principalHash = 3;
    zeroCaptureControlStatus(&status);
    EXPECT_EQ(status.held, 0u);
    EXPECT_EQ(status.leaseGeneration, 0u);
    EXPECT_EQ(status.principalHash, 0u);
}

TEST(MonitorCaptureControl, LiveOkWithoutLeaseProjectsNoLease)
{
    VNX::Monitor::MonitorCaptureControlStatus status {};
    const auto state = projectCaptureControlStatus(VN_OK, &status);
    EXPECT_EQ(state.projection, MonitorLeaseProjection::NoLease);
    EXPECT_EQ(state.queryResult, VN_OK);
    EXPECT_FALSE(canApply(state));
    EXPECT_FALSE(intentAllowed(state));
}

TEST(MonitorCaptureControl, HeldProjectsUnknownHolderNotOwner)
{
    VNX::Monitor::MonitorCaptureControlStatus status {};
    status.held = 1;
    status.leaseGeneration = 12;
    status.principalHash = 99;
    const auto state = projectCaptureControlStatus(VN_OK, &status);
    EXPECT_EQ(state.projection, MonitorLeaseProjection::LeaseHeldByUnknown);
    EXPECT_EQ(state.leaseGeneration, 12u);
    EXPECT_EQ(state.principalHash, 99u);
    EXPECT_FALSE(canApply(state));

    const auto mine = projectCaptureControlStatus(VN_OK, &status, 99, 12);
    EXPECT_EQ(mine.projection, MonitorLeaseProjection::LeaseHeldByThisSession);
    EXPECT_TRUE(canApply(mine));

    const auto staleLease = projectCaptureControlStatus(VN_OK, &status, 99, 11);
    EXPECT_EQ(staleLease.projection, MonitorLeaseProjection::LeaseHeldByUnknown);
    EXPECT_FALSE(canApply(staleLease));
}

TEST(MonitorCaptureControl, QueryFailuresAreUnavailableNotNoLease)
{
    VNX::Monitor::MonitorCaptureControlStatus status {};
    status.held = 1;
    const auto stale = projectCaptureControlStatus(VN_MONITOR_STALE_SESSION, &status);
    EXPECT_EQ(stale.projection, MonitorLeaseProjection::Unavailable);
    EXPECT_EQ(stale.queryResult, VN_MONITOR_STALE_SESSION);
    EXPECT_FALSE(canApply(stale));

    const auto missing = projectCaptureControlStatus(VN_NOT_INITIALIZED, nullptr);
    EXPECT_EQ(missing.projection, MonitorLeaseProjection::Unavailable);
    EXPECT_EQ(missing.queryResult, VN_NOT_INITIALIZED);
    EXPECT_FALSE(canApply(missing));
}

TEST(MonitorCaptureControl, HideWithoutIdDoesNotMutateIntentOrMask)
{
    MonitorMeasurementIntent current;
    current.measurementIds = {7, 9};
    current.subscriptionMask = 0xABCDu;
    const auto next = hidePanelMeasurement(current, std::nullopt);
    EXPECT_EQ(next.measurementIds, current.measurementIds);
    EXPECT_EQ(next.subscriptionMask, 0xABCDu);
}

TEST(MonitorCaptureControl, HideKnownIdLeavesSubscriptionMask)
{
    MonitorMeasurementIntent current;
    current.measurementIds = {7, 9, 11};
    current.subscriptionMask = 0xF0F0u;
    const auto next = hidePanelMeasurement(current, uint64_t{9});
    ASSERT_EQ(next.measurementIds.size(), 2u);
    EXPECT_EQ(next.measurementIds[0], 7u);
    EXPECT_EQ(next.measurementIds[1], 11u);
    EXPECT_EQ(next.subscriptionMask, 0xF0F0u);
}

TEST(MonitorCaptureControl, MonitorPanelsDoNotRequireBuiltinLayout)
{
    EXPECT_EQ(VNX::BuiltinLayouts::findByToken(QStringLiteral("monitor_layout")), nullptr);
    EXPECT_FALSE(VNX::monitorPanelTypeIds().isEmpty());
}

TEST(MonitorCaptureControl, UnavailableKeepsStaleAndDeniedReason)
{
    MonitorCaptureControlState stale;
    stale.projection = MonitorLeaseProjection::Unavailable;
    stale.queryResult = VN_MONITOR_STALE_LEASE;
    EXPECT_FALSE(canApply(stale));

    MonitorCaptureControlState denied;
    denied.projection = MonitorLeaseProjection::Unavailable;
    denied.queryResult = VN_MONITOR_PERMISSION_DENIED;
    EXPECT_FALSE(canApply(denied));
}

TEST(MonitorCaptureControl, RefreshQueriesStatusWithoutAcquire)
{
    VnConfig config {};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);

    MonitorAdapter adapter(syncHost());
    MonitorCaptureControlHost host(&adapter);
    const auto state = host.refresh();
    EXPECT_EQ(state.projection, MonitorLeaseProjection::NoLease);
    EXPECT_EQ(state.queryResult, VN_OK);
    EXPECT_EQ(state.held, 0u);
    EXPECT_FALSE(canApply(state));
    EXPECT_TRUE(host.tracesContain("Vn_MonitorOpenSession"));
    EXPECT_TRUE(host.tracesContain("Vn_MonitorGetCaptureControlStatus"));
    EXPECT_FALSE(host.tracesContain("Vn_MonitorAcquireCaptureControl"));
    EXPECT_FALSE(host.tracesContain("Vn_MonitorReleaseCaptureControl"));
    EXPECT_FALSE(host.tracesContain("Vn_MonitorApplyCapturePlan"));
    EXPECT_FALSE(host.tracesContain("Vn_MonitorStopCapture"));

    host.closeSession();
    EXPECT_TRUE(host.tracesContain("Vn_MonitorCloseSession"));
    EXPECT_FALSE(host.tracesContain("Vn_MonitorStopCapture"));
    EXPECT_FALSE(adapter.hasOpenSession());

    Vn_Term();
}

TEST(MonitorCaptureControl, LocalCaptureOwnerDoesNotClaimRemoteConnection)
{
    VnConfig config {};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);

    MonitorAdapter adapter(syncHost());
    MonitorCaptureControlHost host(&adapter);
    host.openLayout();
    VNX::Monitor::MonitorCaptureControlStatus acquired {};
    ASSERT_EQ(adapter.acquireCaptureControl(adapter.sessionHandle(), &acquired), VN_OK);
    const auto state = host.refresh();
    const auto remote = host.lastRemoteState();
    EXPECT_EQ(state.queryResult, VN_OK);
    EXPECT_TRUE(canApply(state));
    EXPECT_NE(adapter.sessionGeneration(), 0u);
    EXPECT_EQ(remote.face, VNX::Monitor::MonitorRemoteFace::Unsupported);
    EXPECT_EQ(remote.permissionMask, 0u);
    EXPECT_EQ(remote.securityProfile, 0u);
    EXPECT_EQ(remote.leaseGeneration, 0u);

    VnRemoteConfig remoteConfig {};
    remoteConfig.structSize = sizeof(remoteConfig);
    remoteConfig.bindAddress = "127.0.0.1";
    ASSERT_EQ(Vn_RemoteStart(&remoteConfig), VN_OK);
    host.refresh();
    EXPECT_EQ(host.lastRemoteState().face, VNX::Monitor::MonitorRemoteFace::Unsupported);
    EXPECT_TRUE(canApply(host.lastState()));
    EXPECT_EQ(Vn_RemoteStop(), VN_OK);

    host.leaveLayout();
    Vn_Term();
}

TEST(MonitorCaptureControl, RefreshWithoutEngineIsUnavailableNotLease)
{
    MonitorAdapter adapter(syncHost());
    MonitorCaptureControlHost host(&adapter);
    const auto state = host.refresh();
    EXPECT_EQ(state.projection, MonitorLeaseProjection::Unavailable);
    EXPECT_EQ(state.queryResult, VN_NOT_INITIALIZED);
    EXPECT_FALSE(canApply(state));
    EXPECT_FALSE(host.tracesContain("Vn_MonitorAcquireCaptureControl"));
    EXPECT_FALSE(host.tracesContain("Vn_MonitorStopCapture"));
}

TEST(MonitorCaptureControl, CloseSessionClosesOpenedSession)
{
    VnConfig config {};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);

    MonitorAdapter adapter(syncHost());
    MonitorCaptureControlHost host(&adapter);
    host.refresh();
    ASSERT_TRUE(adapter.hasOpenSession());
    host.closeSession();
    EXPECT_FALSE(adapter.hasOpenSession());
    EXPECT_TRUE(host.tracesContain("Vn_MonitorCloseSession"));
    EXPECT_FALSE(host.tracesContain("Vn_MonitorStopCapture"));

    Vn_Term();
}

TEST(MonitorCaptureControl, RepeatedRefreshKeepsExistingSession)
{
    VnConfig config {};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);

    MonitorAdapter adapter(syncHost());
    MonitorCaptureControlHost host(&adapter);
    host.refresh();
    ASSERT_TRUE(host.hasOpenSession());
    const auto handle = adapter.sessionHandle();
    host.refresh();
    EXPECT_TRUE(host.hasOpenSession());
    EXPECT_EQ(adapter.sessionHandle(), handle);
    EXPECT_FALSE(host.tracesContain("Vn_MonitorStopCapture"));

    Vn_Term();
}

TEST(MonitorCaptureControl, SessionRefreshSourcesDoNotAcquireOrStop)
{
    const char* files[] = {
        "Source/MainWindow.cpp",
        "Source/Core/LayoutController.cpp",
        "Source/Monitor/MonitorCaptureControl.cpp",
        "Source/Monitor/MonitorCaptureControlHost.cpp",
        "Source/Widgets/Monitor/MonitorPerformancePanel.cpp",
        "Source/Widgets/Monitor/MonitorPlaceholderPage.cpp",
        "Source/Widgets/Monitor/MonitorCaptureControlStrip.cpp",
    };
    for (const char* file : files)
    {
        const std::string path = std::string(HARKEN_STUDIO_SOURCE_DIR) + "/" + file;
        std::ifstream in(path);
        ASSERT_TRUE(in) << path;
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        EXPECT_EQ(text.find("Vn_MonitorAcquireCaptureControl"), std::string::npos) << path;
        EXPECT_EQ(text.find("Vn_MonitorReleaseCaptureControl"), std::string::npos) << path;
        EXPECT_EQ(text.find("Vn_MonitorStopCapture"), std::string::npos) << path;
        EXPECT_EQ(text.find("Vn_MonitorApplyCapturePlan"), std::string::npos) << path;
    }
}
