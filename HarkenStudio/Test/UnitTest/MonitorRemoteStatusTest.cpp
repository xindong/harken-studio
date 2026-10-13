#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorCaptureControl.h"
#include "Monitor/MonitorRemoteAllowlist.h"
#include "Monitor/MonitorRemoteStatus.h"
#include "Widgets/Monitor/MonitorPerformancePanel.h"
#include "Widgets/Monitor/MonitorRemoteStatusStrip.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QPushButton>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using VNX::Monitor::MonitorCaptureControlState;
using VNX::Monitor::MonitorLeaseProjection;
using VNX::Monitor::MonitorRemoteFace;
using VNX::Monitor::MonitorRemoteStatusInput;
using VNX::Monitor::allowlistRejectsMutation;
using VNX::Monitor::connectedHasControl;
using VNX::Monitor::isMonitorRemoteObserveName;
using VNX::Monitor::permissionDeniedReason;
using VNX::Monitor::permissionGranted;
using VNX::Monitor::recordDeniedReason;
using VNX::Monitor::productionObserveAllowlistNames;
using VNX::Monitor::projectCaptureControlStatus;
using VNX::Monitor::projectRemoteStatus;

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
MonitorCaptureControlState ownerLease(uint64_t generation)
{
    VNX::Monitor::MonitorCaptureControlStatus status {};
    status.held = 1;
    status.leaseGeneration = generation;
    status.principalHash = 9;
    return projectCaptureControlStatus(VN_OK, &status, 9, generation);
}

} // namespace

TEST(MonitorRemoteStatus, MissingContractIsUnsupported)
{
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = false;
    const auto state = projectRemoteStatus(input);
    EXPECT_EQ(state.face, MonitorRemoteFace::Unsupported);
    EXPECT_FALSE(connectedHasControl(state));
}

TEST(MonitorRemoteStatus, HandshakeMismatchIsRejectedNotOwner)
{
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.handshakeCompatible = false;
    input.permissionMask = VN_MONITOR_PERM_CAPTURE_CONTROL;
    input.captureControl = ownerLease(4);
    const auto state = projectRemoteStatus(input);
    EXPECT_EQ(state.face, MonitorRemoteFace::HandshakeRejected);
    EXPECT_FALSE(connectedHasControl(state));
}

TEST(MonitorRemoteStatus, ConnectedWithoutCaptureControlIsObserver)
{
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.permissionMask = VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_RECORD;
    input.reconnectGeneration = 3;
    input.captureControl = ownerLease(3);
    const auto state = projectRemoteStatus(input);
    EXPECT_EQ(state.face, MonitorRemoteFace::ConnectedObserver);
    EXPECT_FALSE(connectedHasControl(state));
    EXPECT_TRUE(permissionGranted(state, VN_MONITOR_PERM_RECORD));
    EXPECT_FALSE(permissionGranted(state, VN_MONITOR_PERM_CAPTURE_CONTROL));
}

TEST(MonitorRemoteStatus, OwnerRequiresCaptureControlAndMatchingLease)
{
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.permissionMask = VN_MONITOR_PERM_CAPTURE_CONTROL | VN_MONITOR_PERM_RECORD;
    input.reconnectGeneration = 8;
    input.captureControl = ownerLease(8);
    const auto state = projectRemoteStatus(input);
    EXPECT_EQ(state.face, MonitorRemoteFace::ConnectedOwner);
    EXPECT_TRUE(connectedHasControl(state));
}

TEST(MonitorRemoteStatus, StaleLeaseGenerationIsObserver)
{
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.permissionMask = VN_MONITOR_PERM_CAPTURE_CONTROL;
    input.reconnectGeneration = 9;
    input.captureControl = ownerLease(8);
    const auto state = projectRemoteStatus(input);
    EXPECT_EQ(state.face, MonitorRemoteFace::ConnectedObserver);
    EXPECT_FALSE(connectedHasControl(state));
}

TEST(MonitorRemoteStatus, IncompatibleQueryIsHandshakeRejectedNotUnsupported)
{
    MonitorRemoteStatusInput input;
    input.queryResult = VN_MONITOR_INCOMPATIBLE;
    input.contractPresent = false;
    input.handshakeCompatible = false;
    input.permissionMask = VN_MONITOR_PERM_CAPTURE_CONTROL | VN_MONITOR_PERM_RECORD;
    const auto state = projectRemoteStatus(input);
    EXPECT_EQ(state.face, MonitorRemoteFace::HandshakeRejected);
    EXPECT_FALSE(connectedHasControl(state));
    EXPECT_EQ(recordDeniedReason(state), "HandshakeRejected");
}

TEST(MonitorRemoteStatus, HandshakeRejectedIsNotStaleLease)
{
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.handshakeCompatible = false;
    input.permissionMask = VN_MONITOR_PERM_CAPTURE_CONTROL | VN_MONITOR_PERM_RECORD;
    input.reconnectGeneration = 4;
    input.captureControl = ownerLease(4);
    const auto state = projectRemoteStatus(input);
    EXPECT_EQ(state.face, MonitorRemoteFace::HandshakeRejected);
    EXPECT_EQ(recordDeniedReason(state), "HandshakeRejected");
}

TEST(MonitorRemoteStatus, RecordDeniedKeepsLeaseSeparateFromPermission)
{
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.permissionMask = VN_MONITOR_PERM_RECORD;
    input.reconnectGeneration = 4;
    input.captureControl = ownerLease(3);
    const auto state = projectRemoteStatus(input);
    EXPECT_TRUE(permissionGranted(state, VN_MONITOR_PERM_RECORD));
    EXPECT_FALSE(connectedHasControl(state));
    EXPECT_EQ(recordDeniedReason(state),
              permissionDeniedReason(VN_MONITOR_PERM_CAPTURE_CONTROL));
}

TEST(MonitorRemoteStatus, StripDoesNotEnableRecordWithoutPermission)
{
    ensureApp();
    VNX::MonitorRemoteStatusStrip strip;
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.permissionMask = VN_MONITOR_PERM_CAPTURE_CONTROL;
    input.reconnectGeneration = 1;
    input.captureControl = ownerLease(1);
    strip.applyState(projectRemoteStatus(input));
    EXPECT_TRUE(strip.controlEnabled());
    EXPECT_FALSE(strip.recordEnabled());
    EXPECT_EQ(strip.recordDeniedReason(),
              QString::fromStdString(permissionDeniedReason(VN_MONITOR_PERM_RECORD)));
}

TEST(MonitorRemoteStatus, StripKeepsStaleLeaseReasonWhenRecordGranted)
{
    ensureApp();
    VNX::MonitorRemoteStatusStrip strip;
    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.permissionMask = VN_MONITOR_PERM_CAPTURE_CONTROL | VN_MONITOR_PERM_RECORD;
    input.reconnectGeneration = 9;
    input.captureControl = ownerLease(8);
    strip.applyState(projectRemoteStatus(input));
    EXPECT_FALSE(strip.controlEnabled());
    EXPECT_FALSE(strip.recordEnabled());
    EXPECT_EQ(strip.recordDeniedReason(), QStringLiteral("StaleLease"));
}

TEST(MonitorRemoteStatus, LocalPerformancePanelHidesUnsupportedRemoteStrip)
{
    ensureApp();
    VNX::MonitorPerformancePanel panel;
    auto* strip = panel.findChild<VNX::MonitorRemoteStatusStrip*>(
        QStringLiteral("MonitorRemoteStatus"));
    ASSERT_NE(strip, nullptr);
    EXPECT_TRUE(strip->isHidden());

    MonitorRemoteStatusInput input;
    input.queryResult = VN_OK;
    input.contractPresent = true;
    input.permissionMask = VN_MONITOR_PERM_CAPTURE_CONTROL | VN_MONITOR_PERM_RECORD;
    input.reconnectGeneration = 2;
    input.captureControl = ownerLease(2);
    panel.applyRemoteStatusState(projectRemoteStatus(input));
    EXPECT_FALSE(strip->isHidden());
    EXPECT_TRUE(panel.remoteControlEnabled());
    EXPECT_TRUE(panel.remoteRecordEnabled());
    EXPECT_TRUE(panel.remoteStatusPermissionText().contains(QStringLiteral("Record=1")));
    panel.applyRemoteStatusState({});
    EXPECT_TRUE(strip->isHidden());
}

TEST(MonitorRemoteStatus, OpeningStripDoesNotEmitAcquire)
{
    ensureApp();
    VNX::MonitorRemoteStatusStrip strip;
    EXPECT_EQ(strip.objectName(), QStringLiteral("MonitorRemoteStatus"));
    EXPECT_FALSE(strip.controlEnabled());
}

TEST(MonitorRemoteStatus, ProductionAllowlistRejectsMutationEmission)
{
    EXPECT_TRUE(isMonitorRemoteObserveName("Vn_MonitorGetRecordingStatus"));
    EXPECT_TRUE(allowlistRejectsMutation("Vn_MonitorStartRecording"));
    EXPECT_TRUE(allowlistRejectsMutation("Vn_MonitorApplyCapturePlan"));
    EXPECT_TRUE(allowlistRejectsMutation("Vn_MonitorAcquireCaptureControl"));
    EXPECT_FALSE(allowlistRejectsMutation("Vn_MonitorGetCaptureControlStatus"));
    const auto names = productionObserveAllowlistNames();
    ASSERT_FALSE(names.empty());
    for (const auto& name : names)
        EXPECT_FALSE(allowlistRejectsMutation(name.c_str())) << name;
}



TEST(MonitorRemoteStatus, DisabledRecordButtonDoesNotDispatchStart)
{
    ensureApp();
    VNX::MonitorRemoteStatusStrip strip;
    int starts = 0;
    QPushButton button(QStringLiteral("StartRecording"));
    button.setEnabled(strip.recordEnabled());
    QObject::connect(&button, &QPushButton::clicked, [&]() {
        if (strip.recordEnabled())
            ++starts;
    });
    button.click();
    EXPECT_EQ(starts, 0);
}

TEST(MonitorRemoteStatus, StatusStripSourceHasNoAcquireCall)
{
    const std::string path = std::string(HARKEN_STUDIO_SOURCE_DIR)
        + "/Source/Widgets/Monitor/MonitorRemoteStatusStrip.cpp";
    std::ifstream in(path);
    ASSERT_TRUE(in) << path;
    const std::string source((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    EXPECT_EQ(source.find("Vn_MonitorAcquireCaptureControl"), std::string::npos);
    EXPECT_EQ(source.find("Vn_MonitorStartRecording"), std::string::npos);
}
