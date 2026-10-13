#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/TestUtil.h"
#include "Monitor/MonitorAdapter.h"
#include "RuntimeBridge/MonitorBridge.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using VNX::Monitor::MonitorAdapter;
using VNX::Monitor::MonitorHostContext;
using VNX::Monitor::MonitorPrincipal;

namespace {

MonitorHostContext syncHost(bool authenticated = true)
{
    MonitorHostContext host;
    host.principal.authenticated = authenticated;
    host.principal.id = authenticated ? "studio-local" : "";
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

TEST(MonitorAdapterRuntime, OpenNullDescIsInvalidArgWithoutHandle)
{
    MonitorAdapter adapter(syncHost());
    const auto opened = adapter.open(nullptr);
    EXPECT_EQ(opened.result, VN_INVALID_ARG);
    EXPECT_EQ(opened.handle, 0u);
    EXPECT_FALSE(adapter.hasOpenSession());
}

TEST(MonitorAdapterRuntime, OpenWithoutPrincipalDoesNotCallRuntime)
{
    MonitorAdapter adapter(syncHost(false));
    auto desc = validDesc();
    const auto opened = adapter.open(&desc);
    EXPECT_EQ(opened.result, VN_MONITOR_PERMISSION_DENIED);
    EXPECT_EQ(opened.handle, 0u);
    EXPECT_FALSE(adapter.hasOpenSession());
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorOpenSession"));
}

TEST(MonitorAdapterRuntime, OpenWithoutEngineIsNotInitialized)
{
    MonitorAdapter adapter(syncHost());
    auto desc = validDesc();
    const auto opened = adapter.open(&desc);
    EXPECT_EQ(opened.result, VN_NOT_INITIALIZED);
    EXPECT_EQ(opened.handle, 0u);
    EXPECT_EQ(opened.epoch, 0u);
    EXPECT_EQ(opened.generation, 0u);
    EXPECT_FALSE(adapter.hasOpenSession());
    EXPECT_FALSE(adapter.cachedPrincipal().has_value());
    EXPECT_TRUE(tracesContain(adapter, "Vn_MonitorOpenSession"));
}

TEST(MonitorAdapterRuntime, OpenWithEnginePresentOpensLiveSession)
{
    VnConfig config {};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);

    MonitorAdapter adapter(syncHost());
    auto desc = validDesc();
    const auto opened = adapter.open(&desc);
    EXPECT_EQ(opened.result, VN_OK);
    EXPECT_NE(opened.handle, 0u);
    EXPECT_NE(opened.epoch, 0u);
    EXPECT_NE(opened.generation, 0u);
    EXPECT_TRUE(adapter.hasOpenSession());
    EXPECT_TRUE(tracesContain(adapter, "Vn_MonitorOpenSession"));
    EXPECT_EQ(adapter.close(opened.handle), VN_OK);
    EXPECT_FALSE(adapter.hasOpenSession());

    Vn_Term();
}

TEST(MonitorAdapterRuntime, PollWithoutSessionIsStale)
{
    MonitorAdapter adapter(syncHost());
    VNX::Monitor::MonitorPollDesc desc {};
    std::vector<uint8_t> buffer(64);
    desc.buffer = buffer.data();
    desc.bufferBytes = buffer.size();
    VNX::Monitor::MonitorPollResult result {};
    EXPECT_EQ(adapter.poll(1, &desc, &result), VN_MONITOR_STALE_SESSION);
    EXPECT_EQ(result.recordsWritten, 0u);
}

TEST(MonitorAdapterRuntime, BridgePreservesSubscriptionBaselineOutput)
{
    namespace Monitoring = HarkenEditor::Bridge::Monitoring;
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(engine.InitResult(), VN_OK);
    VNX::Monitor::MonitorSessionDesc desc{};
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_STATE | VN_MONITOR_STREAM_TIME_SERIES;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_OBSERVE_MEASUREMENTS;
    desc.consumerQueueBytes = 65536;
    uint64_t handle = 0;
    ASSERT_EQ(Monitoring::openSession(&desc, &handle), VN_OK);

    VNX::Monitor::MonitorStreamBaseline baselines[2]{};
    VNX::Monitor::MonitorSubscriptionStatus status{};
    status.baselines = baselines;
    status.baselineCapacity = 1;
    EXPECT_EQ(Monitoring::getSubscriptionStatus(handle, &status), VN_MONITOR_BUFFER_TOO_SMALL);
    EXPECT_EQ(status.baselineCount, 2u);
    EXPECT_EQ(status.baselines, baselines);
    status.baselineCapacity = 2;
    ASSERT_EQ(Monitoring::getSubscriptionStatus(handle, &status), VN_OK);
    EXPECT_EQ(status.baselineCount, 2u);
    EXPECT_EQ(status.baselineCapacity, 2u);
    EXPECT_EQ(status.baselines, baselines);
    EXPECT_EQ(baselines[0].streamKind, VN_MONITOR_STREAM_TIME_SERIES);
    EXPECT_EQ(baselines[1].streamKind, VN_MONITOR_STREAM_STATE);
    EXPECT_NE(baselines[0].streamId, 0u);
    EXPECT_NE(baselines[1].streamId, 0u);
    EXPECT_EQ(Monitoring::closeSession(handle), VN_OK);
}

TEST(MonitorAdapterRuntime, CloseWithoutSessionIsStale)
{
    MonitorAdapter adapter(syncHost());
    EXPECT_EQ(adapter.close(1), VN_MONITOR_STALE_SESSION);
}

TEST(MonitorAdapterRuntime, ReadCapabilitiesDoesNotCreateSession)
{
    MonitorAdapter adapter(syncHost());
    VNX::Monitor::MonitorCapabilities caps {};
    const VnResult result = adapter.readCapabilities(&caps);
    EXPECT_TRUE(result == VN_MONITOR_UNSUPPORTED || result == VN_NOT_INITIALIZED);
    EXPECT_FALSE(adapter.hasOpenSession());
    EXPECT_EQ(adapter.sessionEpoch(), 0u);
    EXPECT_EQ(adapter.sessionGeneration(), 0u);
}

TEST(MonitorAdapterRuntime, ConsumerStatusZerosCallerBufferOnStale)
{
    MonitorAdapter adapter(syncHost());
    VNX::Monitor::MonitorDropSummary summary {};
    summary.structSize = sizeof(summary);
    summary.recordCount = 99;
    summary.byteCount = 42;
    uint32_t required = 7;
    EXPECT_EQ(adapter.getConsumerStatus(1, &summary, 1, &required), VN_MONITOR_STALE_SESSION);
    EXPECT_EQ(required, 0u);
    EXPECT_EQ(summary.recordCount, 0u);
    EXPECT_EQ(summary.byteCount, 0u);
    EXPECT_EQ(summary.structSize, sizeof(summary));
}

TEST(MonitorAdapterRuntime, SourcesDoNotLinkWidgetsOrOldDiagnostics)
{
    const char* files[] = {
        "Source/Monitor/MonitorAdapter.cpp",
        "Source/Monitor/MonitorAdapter.h",
        "Source/Monitor/MonitorHostContext.h",
        "Source/Monitor/CMakeLists.txt",
    };
    for (const char* file : files)
    {
        const std::string path = std::string(HARKEN_STUDIO_SOURCE_DIR) + "/" + file;
        std::ifstream in(path);
        ASSERT_TRUE(in) << path;
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        EXPECT_EQ(text.find("Qt6::Widgets"), std::string::npos) << path;
        EXPECT_EQ(text.find("DiagnosticsBridge"), std::string::npos) << path;
        EXPECT_EQ(text.find("tapPost"), std::string::npos) << path;
        EXPECT_EQ(text.find("setPrincipal"), std::string::npos) << path;
        EXPECT_EQ(text.find("harken_editor_foundation"), std::string::npos) << path;
    }
}

TEST(MonitorAdapterModelOnly, AsyncPosterDoesNotRunWorkOrUaf)
{
    MonitorHostContext host = syncHost();
    host.logicThreadPoster = [](std::function<void()>) {
        // Queue-and-return: must not execute the captured stack lambda.
    };
    MonitorAdapter adapter(host);
    auto desc = validDesc();
    const auto opened = adapter.open(&desc);
    EXPECT_EQ(opened.result, VN_MONITOR_WRONG_THREAD);
    EXPECT_EQ(opened.handle, 0u);
    EXPECT_FALSE(adapter.hasOpenSession());
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorOpenSession"));
}

TEST(MonitorAdapterModelOnly, MissingPosterIsWrongThread)
{
    MonitorHostContext host = syncHost();
    host.logicThreadPoster = nullptr;
    MonitorAdapter adapter(host);
    auto desc = validDesc();
    const auto opened = adapter.open(&desc);
    EXPECT_EQ(opened.result, VN_MONITOR_WRONG_THREAD);
    EXPECT_FALSE(adapter.hasOpenSession());
}

TEST(MonitorAdapterModelOnly, ControlCallsUsePosterThread)
{
    const auto testThread = std::this_thread::get_id();
    MonitorHostContext host = syncHost();
    host.logicThreadPoster = [testThread](std::function<void()> work) {
        EXPECT_EQ(std::this_thread::get_id(), testThread);
        work();
    };
    MonitorAdapter adapter(host);
    auto desc = validDesc();
    adapter.open(&desc);
    ASSERT_FALSE(adapter.callTrace().empty());
    EXPECT_EQ(adapter.callTrace().front().entry, std::string("Vn_MonitorOpenSession"));
    EXPECT_EQ(adapter.callTrace().front().threadId, testThread);
}

TEST(MonitorAdapterModelOnly, PollNullBufferWithBytesIsInvalidArg)
{
    MonitorAdapter adapter(syncHost());
    VNX::Monitor::MonitorPollDesc desc {};
    desc.buffer = nullptr;
    desc.bufferBytes = 64;
    VNX::Monitor::MonitorPollResult result {};
    EXPECT_EQ(adapter.poll(1, &desc, &result), VN_INVALID_ARG);
    EXPECT_EQ(result.recordsWritten, 0u);
}

TEST(MonitorAdapterRuntime, CaptureControlStatusStaleZerosCallerBufferWithoutTrace)
{
    MonitorAdapter adapter(syncHost());
    VNX::Monitor::MonitorCaptureControlStatus status {};
    status.held = 1;
    status.leaseGeneration = 9;
    status.principalHash = 11;
    EXPECT_EQ(adapter.getCaptureControlStatus(1, &status), VN_MONITOR_STALE_SESSION);
    EXPECT_EQ(status.held, 0u);
    EXPECT_EQ(status.leaseGeneration, 0u);
    EXPECT_EQ(status.principalHash, 0u);
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorGetCaptureControlStatus"));
}

TEST(MonitorAdapterRuntime, CaptureControlStatusNullOutputIsInvalidArgWithoutTrace)
{
    MonitorAdapter adapter(syncHost());
    EXPECT_EQ(adapter.getCaptureControlStatus(1, nullptr), VN_INVALID_ARG);
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorGetCaptureControlStatus"));
}

TEST(MonitorAdapterRuntime, OpenSessionQueryHasNoLeaseAndDoesNotAcquire)
{
    VnConfig config {};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);

    MonitorAdapter adapter(syncHost());
    auto desc = validDesc();
    const auto opened = adapter.open(&desc);
    ASSERT_EQ(opened.result, VN_OK);
    ASSERT_NE(opened.handle, 0u);

    VNX::Monitor::MonitorCaptureControlStatus status {};
    status.held = 1;
    EXPECT_EQ(adapter.getCaptureControlStatus(opened.handle, &status), VN_OK);
    EXPECT_EQ(status.held, 0u);
    EXPECT_EQ(status.leaseGeneration, 0u);
    EXPECT_EQ(status.principalHash, 0u);
    EXPECT_TRUE(tracesContain(adapter, "Vn_MonitorGetCaptureControlStatus"));
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorAcquireCaptureControl"));
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorReleaseCaptureControl"));
    EXPECT_EQ(adapter.close(opened.handle), VN_OK);

    Vn_Term();
}

TEST(MonitorAdapterRuntime, CaptureControlStatusAfterTermIsNotInitializedAndZeroed)
{
    VnConfig config {};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);

    MonitorAdapter adapter(syncHost());
    auto desc = validDesc();
    const auto opened = adapter.open(&desc);
    ASSERT_EQ(opened.result, VN_OK);
    Vn_Term();

    VNX::Monitor::MonitorCaptureControlStatus status {};
    status.held = 1;
    status.leaseGeneration = 4;
    EXPECT_EQ(adapter.getCaptureControlStatus(opened.handle, &status), VN_NOT_INITIALIZED);
    EXPECT_EQ(status.held, 0u);
    EXPECT_EQ(status.leaseGeneration, 0u);
}

TEST(MonitorAdapterModelOnly, CaptureControlStatusWithoutPosterIsWrongThread)
{
    MonitorHostContext host = syncHost();
    host.logicThreadPoster = nullptr;
    MonitorAdapter adapter(host);
    VNX::Monitor::MonitorCaptureControlStatus status {};
    status.held = 1;
    EXPECT_EQ(adapter.getCaptureControlStatus(1, &status), VN_MONITOR_WRONG_THREAD);
    EXPECT_EQ(status.held, 0u);
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorGetCaptureControlStatus"));
}

TEST(MonitorAdapterRuntime, RecordingCallsWithoutSessionAreStaleAndDoNotTrace)
{
    MonitorAdapter adapter(syncHost());
    VNX::Monitor::MonitorRecordingDesc desc {};
    desc.utf8Path = nullptr;
    desc.fileByteBudget = 0;
    EXPECT_EQ(adapter.startRecording(1, 1, &desc), VN_MONITOR_STALE_SESSION);
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorStartRecording"));
    EXPECT_EQ(adapter.stopRecording(1, 1), VN_MONITOR_STALE_SESSION);
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorStopRecording"));

    VNX::Monitor::MonitorRecordingStatus status {};
    status.state = VN_MONITOR_RECORDING_STATE_ACTIVE;
    status.fileBytes = 9;
    EXPECT_EQ(adapter.getRecordingStatus(1, &status), VN_MONITOR_STALE_SESSION);
    EXPECT_EQ(status.state, 0u);
    EXPECT_EQ(status.fileBytes, 0u);
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorGetRecordingStatus"));
}

TEST(MonitorAdapterRuntime, RecordingNullDescIsInvalidArgWithoutTrace)
{
    MonitorAdapter adapter(syncHost());
    EXPECT_EQ(adapter.startRecording(1, 1, nullptr), VN_INVALID_ARG);
    EXPECT_FALSE(tracesContain(adapter, "Vn_MonitorStartRecording"));
}
