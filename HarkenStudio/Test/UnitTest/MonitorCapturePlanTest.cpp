#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/CapturePlanIntent.h"
#include "Monitor/MonitorAdapter.h"
#include "Monitor/MonitorCaptureControl.h"
#include "Monitor/MonitorCaptureControlHost.h"
#include "Monitor/MonitorHostContext.h"
#include "Widgets/Monitor/MonitorCaptureControlStrip.h"
#include "Widgets/Monitor/MonitorPerformancePanel.h"
#include "Core/WorkspaceWindowTypes.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>

#include <cstring>
#include <string>
#include <vector>

using VNX::Monitor::CaptureBudgetPreview;
using VNX::Monitor::CapturePlanFace;
using VNX::Monitor::CapturePlanIntent;
using VNX::Monitor::MonitorAdapter;
using VNX::Monitor::MonitorCaptureControlHost;
using VNX::Monitor::MonitorHostContext;
using VNX::Monitor::MonitorLeaseProjection;
using VNX::Monitor::canApply;
using VNX::Monitor::generationUnchanged;
using VNX::Monitor::keepPreviousPreview;
using VNX::Monitor::previewFromEffective;
using VNX::Monitor::projectCaptureControlStatus;

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

VNX::Monitor::MonitorSessionDesc controlDesc()
{
    VNX::Monitor::MonitorSessionDesc desc {};
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_CAPTURE_CONTROL;
    desc.consumerQueueBytes = 65536;
    return desc;
}

VNX::Monitor::MonitorCapturePlan emptyAudioPlan()
{
    VNX::Monitor::MonitorCapturePlan plan {};
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.bytesPerSecondBudget = 0;
    return plan;
}

struct EngineGuard
{
    EngineGuard()
    {
        VnConfig config {};
        config.structSize = sizeof(config);
        config.deviceMode = VN_DEVICE_NULL;
        init = Vn_Init(&config);
    }
    ~EngineGuard()
    {
        if (init == VN_OK)
            Vn_Term();
    }
    VnResult init = VN_NOT_INITIALIZED;
};

} // namespace

TEST(MonitorCapturePlan, OwnerHashMatchesAcquireNotDisplayId)
{
    EngineGuard engine;
    ASSERT_EQ(engine.init, VN_OK);
    MonitorAdapter adapter(syncHost());
    auto desc = controlDesc();
    const auto opened = adapter.open(&desc);
    ASSERT_EQ(opened.result, VN_OK);

    VNX::Monitor::MonitorCaptureControlStatus status {};
    ASSERT_EQ(adapter.getCaptureControlStatus(opened.handle, &status), VN_OK);
    auto observed = projectCaptureControlStatus(
        VN_OK, &status, adapter.acquiredPrincipalHash(), adapter.acquiredLeaseGeneration());
    EXPECT_EQ(observed.projection, MonitorLeaseProjection::NoLease);
    EXPECT_FALSE(canApply(observed));

    ASSERT_EQ(adapter.acquireCaptureControl(opened.handle, &status), VN_OK);
    EXPECT_NE(adapter.acquiredPrincipalHash(), 0u);
    EXPECT_NE(adapter.acquiredLeaseGeneration(), 0u);
    observed = projectCaptureControlStatus(
        VN_OK, &status, adapter.acquiredPrincipalHash(), adapter.acquiredLeaseGeneration());
    EXPECT_EQ(observed.projection, MonitorLeaseProjection::LeaseHeldByThisSession);
    EXPECT_TRUE(canApply(observed));

    auto staleGen = projectCaptureControlStatus(
        VN_OK, &status, adapter.acquiredPrincipalHash(), status.leaseGeneration + 1);
    EXPECT_EQ(staleGen.projection, MonitorLeaseProjection::LeaseHeldByUnknown);
    EXPECT_FALSE(canApply(staleGen));

    adapter.close(opened.handle);
}

TEST(MonitorCapturePlan, RefreshStillDoesNotAcquire)
{
    EngineGuard engine;
    ASSERT_EQ(engine.init, VN_OK);
    MonitorAdapter adapter(syncHost());
    MonitorCaptureControlHost host(&adapter);
    const auto state = host.refresh();
    EXPECT_EQ(state.projection, MonitorLeaseProjection::NoLease);
    EXPECT_FALSE(canApply(state));
    EXPECT_FALSE(host.tracesContain("Vn_MonitorAcquireCaptureControl"));
    EXPECT_FALSE(host.tracesContain("Vn_MonitorApplyCapturePlan"));
    host.closeSession();
}

TEST(MonitorCapturePlan, ValidateBudgetExceededKeepsGeneration)
{
    EngineGuard engine;
    ASSERT_EQ(engine.init, VN_OK);
    MonitorAdapter adapter(syncHost());
    auto desc = controlDesc();
    const auto opened = adapter.open(&desc);
    ASSERT_EQ(opened.result, VN_OK);
    VNX::Monitor::MonitorCaptureControlStatus status {};
    ASSERT_EQ(adapter.acquireCaptureControl(opened.handle, &status), VN_OK);

    CaptureBudgetPreview before;
    before.capturePlanGeneration = 1;
    auto plan = emptyAudioPlan();
    plan.bytesPerSecondBudget = 1;
    VNX::Monitor::MonitorEffectiveCapturePlan accepted {};
    const VnResult validated =
        adapter.validateCapturePlan(opened.handle, status.leaseGeneration, &plan, &accepted);
    CapturePlanIntent intent;
    intent.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    const auto preview = keepPreviousPreview(before, validated, intent);
    EXPECT_EQ(validated, VN_MONITOR_BUDGET_EXCEEDED);
    EXPECT_EQ(preview.capturePlanGeneration, 1u);
    EXPECT_TRUE(generationUnchanged(before, preview));

    VNX::Monitor::MonitorEffectiveCapturePlan effective {};
    ASSERT_EQ(adapter.getEffectiveCapturePlan(opened.handle, 0, &effective), VN_OK);
    EXPECT_EQ(effective.capturePlanGeneration, 1u);
    EXPECT_EQ(effective.accepted.measurementCount, 0u);

    adapter.close(opened.handle);
}

TEST(MonitorCapturePlan, EmptyIntentApplyIsNotStop)
{
    EngineGuard engine;
    ASSERT_EQ(engine.init, VN_OK);
    MonitorAdapter adapter(syncHost());
    auto desc = controlDesc();
    const auto opened = adapter.open(&desc);
    ASSERT_EQ(opened.result, VN_OK);
    VNX::Monitor::MonitorCaptureControlStatus status {};
    ASSERT_EQ(adapter.acquireCaptureControl(opened.handle, &status), VN_OK);

    auto plan = emptyAudioPlan();
    VNX::Monitor::MonitorEffectiveCapturePlan accepted {};
    const VnResult applied =
        adapter.applyCapturePlan(opened.handle, status.leaseGeneration, &plan, &accepted);
    EXPECT_EQ(applied, VN_OK);
    EXPECT_NE(accepted.activationState, static_cast<uint32_t>(VN_MONITOR_PLAN_RETIRED));
    EXPECT_EQ(accepted.accepted.measurementCount, 0u);
    EXPECT_EQ(accepted.accepted.streamKindMask, VN_MONITOR_STREAM_OPTIONAL_AUDIO);

    VNX::Monitor::MonitorEffectiveCapturePlan stopped {};
    ASSERT_EQ(adapter.stopCapture(opened.handle, status.leaseGeneration, &stopped), VN_OK);
    EXPECT_NE(stopped.accepted.streamKindMask, accepted.accepted.streamKindMask);

    adapter.close(opened.handle);
    EXPECT_EQ(adapter.acquiredPrincipalHash(), 0u);
    EXPECT_EQ(adapter.acquiredLeaseGeneration(), 0u);
}

TEST(MonitorCapturePlan, ObserverValidateApplyStopAreDenied)
{
    EngineGuard engine;
    ASSERT_EQ(engine.init, VN_OK);
    MonitorAdapter owner(syncHost());
    MonitorAdapter observer(syncHost());
    auto desc = controlDesc();
    const auto owned = owner.open(&desc);
    ASSERT_EQ(owned.result, VN_OK);
    const auto watched = observer.open(&desc);
    ASSERT_EQ(watched.result, VN_OK);

    VNX::Monitor::MonitorCaptureControlStatus status {};
    ASSERT_EQ(owner.acquireCaptureControl(owned.handle, &status), VN_OK);

    auto plan = emptyAudioPlan();
    VNX::Monitor::MonitorEffectiveCapturePlan accepted {};
    EXPECT_EQ(
        observer.validateCapturePlan(watched.handle, status.leaseGeneration, &plan, &accepted),
        VN_MONITOR_STALE_LEASE);
    EXPECT_EQ(accepted.capturePlanGeneration, 0u);
    EXPECT_EQ(
        observer.applyCapturePlan(watched.handle, status.leaseGeneration, &plan, &accepted),
        VN_MONITOR_STALE_LEASE);
    EXPECT_EQ(
        observer.stopCapture(watched.handle, status.leaseGeneration, &accepted),
        VN_MONITOR_STALE_LEASE);
    EXPECT_EQ(
        observer.releaseCaptureControl(watched.handle, status.leaseGeneration),
        VN_MONITOR_STALE_LEASE);

    VNX::Monitor::MonitorEffectiveCapturePlan effective {};
    ASSERT_EQ(owner.getEffectiveCapturePlan(owned.handle, 0, &effective), VN_OK);
    EXPECT_EQ(effective.capturePlanGeneration, 1u);

    owner.close(owned.handle);
    observer.close(watched.handle);
}

TEST(MonitorCapturePlan, CloseClearsHashEvenIfSessionAlreadyGone)
{
    EngineGuard engine;
    ASSERT_EQ(engine.init, VN_OK);
    MonitorAdapter adapter(syncHost());
    auto desc = controlDesc();
    const auto opened = adapter.open(&desc);
    ASSERT_EQ(opened.result, VN_OK);
    VNX::Monitor::MonitorCaptureControlStatus status {};
    ASSERT_EQ(adapter.acquireCaptureControl(opened.handle, &status), VN_OK);
    EXPECT_NE(adapter.acquiredPrincipalHash(), 0u);

    ASSERT_EQ(Vn_MonitorCloseSession(opened.handle), VN_OK);
    EXPECT_EQ(adapter.close(opened.handle), VN_MONITOR_STALE_SESSION);
    EXPECT_EQ(adapter.acquiredPrincipalHash(), 0u);
    EXPECT_EQ(adapter.acquiredLeaseGeneration(), 0u);
    EXPECT_FALSE(adapter.hasOpenSession());
}

TEST(MonitorCapturePlan, StripShowsPreviewAndIpcName)
{
    ensureApp();
    VNX::MonitorCaptureControlStrip strip;
    EXPECT_EQ(strip.objectName(), QStringLiteral("MonitorCapturePlanBar"));
    CaptureBudgetPreview preview;
    preview.face = CapturePlanFace::EmptyMeasurement;
    preview.chargesAudioChunk = true;
    strip.applyPreview(preview);

    const QStringList ipc = VNX::ipcKnownPanelObjectNames();
    EXPECT_TRUE(ipc.contains(QStringLiteral("MonitorCapturePlanBar")));
    QLabel child(&strip);
    EXPECT_EQ(VNX::ipcFocusedWidgetKnownPanelName(&child), QStringLiteral("MonitorCapturePlanBar"));
    QWidget unknown;
    unknown.setObjectName(QStringLiteral("NotAKnownPanel"));
    EXPECT_TRUE(VNX::ipcFocusedWidgetKnownPanelName(&unknown).isEmpty());
}

TEST(MonitorCapturePlan, PerformancePanelApplyEnabledOnlyForThisSession)
{
    ensureApp();
    VNX::MonitorPerformancePanel panel;
    VNX::Monitor::MonitorCaptureControlState none;
    none.projection = MonitorLeaseProjection::NoLease;
    none.queryResult = VN_OK;
    panel.applyCaptureControlState(none);
    EXPECT_FALSE(panel.capturePlanApplyEnabled());

    VNX::Monitor::MonitorCaptureControlState mine;
    mine.projection = MonitorLeaseProjection::LeaseHeldByThisSession;
    mine.queryResult = VN_OK;
    mine.leaseGeneration = 3;
    panel.applyCaptureControlState(mine);
    EXPECT_TRUE(panel.capturePlanApplyEnabled());
}
