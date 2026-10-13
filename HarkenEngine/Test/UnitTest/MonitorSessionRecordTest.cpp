#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "Common/TestUtil.h"
#include "monitor_session_codec.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

VnMonitorSessionDesc MakeRecordDesc() {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_STATE;
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_CAPTURE_CONTROL |
        VN_MONITOR_PERM_RECORD;
    desc.consumerQueueBytes = 8192;
    return desc;
}

VnMonitorRecordingDesc MakeRecordingDesc(const char* path, std::uint64_t budget) {
    VnMonitorRecordingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.utf8Path = path;
    desc.fileByteBudget = budget;
    return desc;
}

VnMonitorRecordingStatus MakeStatus() {
    VnMonitorRecordingStatus status{};
    status.structSize = sizeof(status);
    return status;
}

std::string TempSessionPath(const char* name) {
    const auto dir =
        std::filesystem::temp_directory_path() / "harken-monitor-m41";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / name).string();
}

void RemoveIfExists(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

std::vector<std::uint8_t> ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << path;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

VnResult Acquire(VnMonitorSessionHandle handle,
                 VnMonitorCaptureControlStatus* status) {
    status->structSize = sizeof(*status);
    return Vn_MonitorAcquireCaptureControl(handle, status);
}

VnMonitorRecordingStatus StopAndWait(VnMonitorSessionHandle handle,
                                     std::uint64_t leaseGeneration) {
    EXPECT_EQ(VN_OK, Vn_MonitorStopRecording(handle, leaseGeneration));
    VnMonitorRecordingStatus status = MakeStatus();
    const auto start = std::chrono::steady_clock::now();
    do {
        status = MakeStatus();
        EXPECT_EQ(VN_OK, Vn_MonitorGetRecordingStatus(handle, &status));
        if (status.state == VN_MONITOR_RECORDING_STATE_STOPPED ||
            status.state == VN_MONITOR_RECORDING_STATE_FAILED) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    EXPECT_TRUE(status.state == VN_MONITOR_RECORDING_STATE_STOPPED ||
                status.state == VN_MONITOR_RECORDING_STATE_FAILED)
        << "recording still " << status.state;
    return status;
}

} // namespace

TEST(MonitorSession, RecordStaleHandleDoesNotCreateFile) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::string path = TempSessionPath("stale.vnmon");
    RemoveIfExists(path);
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorStartRecording(0, 0, &desc));
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(MonitorSession, RecordRejectsWithoutPermissionOrLease) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::string path = TempSessionPath("denied.vnmon");
    RemoveIfExists(path);

    VnMonitorSessionDesc observe = MakeRecordDesc();
    observe.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_STATE;
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&observe, &handle));
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    EXPECT_EQ(VN_MONITOR_PERMISSION_DENIED,
              Vn_MonitorStartRecording(handle, 1, &desc));
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));

    VnMonitorSessionDesc record = MakeRecordDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&record, &handle));
    VnMonitorSubscriptionStatus sub{};
    sub.structSize = sizeof(sub);
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(handle, &sub));
    EXPECT_EQ(VN_MONITOR_PERM_RECORD,
              sub.acceptedPermissionMask & VN_MONITOR_PERM_RECORD);
    EXPECT_EQ(0u, sub.unsupportedPermissionMask & VN_MONITOR_PERM_RECORD);
    EXPECT_EQ(VN_MONITOR_STALE_LEASE,
              Vn_MonitorStartRecording(handle, 1, &desc));
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorSession, RecordStartPollStopWritesCommittedFile) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::string path = TempSessionPath("local.vnmon");
    RemoveIfExists(path);

    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc openDesc = MakeRecordDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&openDesc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    auto desc = MakeRecordingDesc(path.c_str(), 8ull * 1024ull * 1024ull);
    ASSERT_EQ(VN_OK,
              Vn_MonitorStartRecording(handle, lease.leaseGeneration, &desc));

    auto status = MakeStatus();
    ASSERT_EQ(VN_OK, Vn_MonitorGetRecordingStatus(handle, &status));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_ACTIVE, status.state);

    std::array<unsigned char, 4096> buffer{};
    VnMonitorPollDesc poll{};
    poll.structSize = sizeof(poll);
    poll.buffer = buffer.data();
    poll.bufferBytes = buffer.size();
    VnMonitorPollResult pollResult{};
    pollResult.structSize = sizeof(pollResult);
    EXPECT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll, &pollResult));

    status = StopAndWait(handle, lease.leaseGeneration);
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, status.state);
    EXPECT_GT(status.fileBytes, 0u);
    EXPECT_GT(status.lastCommittedChunk, 0u);
    EXPECT_EQ(0u, status.recoveryNeeded);

    const auto bytes = ReadAll(path);
    EXPECT_EQ(bytes.size(), status.fileBytes);
    std::vector<VnMonitorSessionCommitView> commits(16);
    VnMonitorDecodedRecord scratch[16]{};
    std::uint8_t skip[4096]{};
    auto scan = vn_monitor_scan_session(bytes.data(), bytes.size(), commits.data(),
                                        16, scratch, 16, skip, sizeof(skip));
    EXPECT_EQ(VN_OK, scan.status);
    EXPECT_GE(scan.commitsWritten, 1u);
    EXPECT_EQ(scan.bytesConsumed, bytes.size());
    EXPECT_EQ(VN_MONITOR_SESSION_FILE_MAGIC, scan.fileMagic);

    EXPECT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll, &pollResult));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    RemoveIfExists(path);
}

TEST(MonitorSession, FailedRecordingStartDoesNotEnableRecorderConsumer) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc openDesc = MakeRecordDesc();
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&openDesc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    const std::string path = TempSessionPath("start-budget-fail.vnmon");
    RemoveIfExists(path);
    auto desc = MakeRecordingDesc(path.c_str(), 1);
    EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED,
              Vn_MonitorStartRecording(handle, lease.leaseGeneration, &desc));
    EXPECT_FALSE(harken::monitor::RecorderConsumerEnabledForTests());
    EXPECT_FALSE(std::filesystem::exists(path));
    harken::monitor::StopDrainForTests();
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(2));
    harken::monitor::DrainOnceForTests();
    EXPECT_EQ(0u, harken::monitor::InternalQueuedCountForTests(0));
    harken::monitor::StartDrainForTests();
    desc.fileByteBudget = 1024 * 1024;
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(handle, lease.leaseGeneration, &desc));
    EXPECT_TRUE(harken::monitor::RecorderConsumerEnabledForTests());
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, StopAndWait(handle, lease.leaseGeneration).state);
    EXPECT_FALSE(harken::monitor::RecorderConsumerEnabledForTests());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    RemoveIfExists(path);
}

TEST(MonitorSession, StoppingIdleRecordingWakesSleepingDrain) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc openDesc = MakeRecordDesc();
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&openDesc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    const std::string path = TempSessionPath("idle-stop.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    for (int attempt = 0; attempt != 4; ++attempt) {
        ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(handle, lease.leaseGeneration, &desc));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, StopAndWait(handle, lease.leaseGeneration).state);
        EXPECT_FALSE(harken::monitor::RecorderConsumerEnabledForTests());
    }
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    RemoveIfExists(path);
}

TEST(MonitorSession, RecordCloseDoesNotStopFile) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::string path = TempSessionPath("close-keeps.vnmon");
    RemoveIfExists(path);

    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc openDesc = MakeRecordDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&openDesc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    auto desc = MakeRecordingDesc(path.c_str(), 8ull * 1024ull * 1024ull);
    ASSERT_EQ(VN_OK,
              Vn_MonitorStartRecording(handle, lease.leaseGeneration, &desc));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));

    auto status = MakeStatus();
    EXPECT_EQ(VN_MONITOR_STALE_SESSION,
              Vn_MonitorGetRecordingStatus(handle, &status));
    EXPECT_TRUE(std::filesystem::exists(path));

    VnMonitorSessionHandle next = 0;
    VnMonitorSessionDesc nextDesc = MakeRecordDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&nextDesc, &next));
    status = MakeStatus();
    ASSERT_EQ(VN_OK, Vn_MonitorGetRecordingStatus(next, &status));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_ACTIVE, status.state);
    VnMonitorCaptureControlStatus nextLease{};
    ASSERT_EQ(VN_OK, Acquire(next, &nextLease));
    status = StopAndWait(next, nextLease.leaseGeneration);
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, status.state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(next));
    RemoveIfExists(path);
}

TEST(MonitorSession, RecordRejectsBudgetOverHardLimitWithoutFile) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::string path = TempSessionPath("over-hard.vnmon");
    RemoveIfExists(path);

    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc openDesc = MakeRecordDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&openDesc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    auto desc = MakeRecordingDesc(path.c_str(), VN_MONITOR_HARD_SESSION_FILE_BYTES + 1);
    EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED,
              Vn_MonitorStartRecording(handle, lease.leaseGeneration, &desc));
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorSession, RecordWriteFailureMarksFailedAndNeedsRecovery) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::string path = TempSessionPath("write-fail.vnmon");
    RemoveIfExists(path);

    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc openDesc = MakeRecordDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&openDesc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    auto desc = MakeRecordingDesc(path.c_str(), 8ull * 1024ull * 1024ull);
    ASSERT_EQ(VN_OK,
              Vn_MonitorStartRecording(handle, lease.leaseGeneration, &desc));
    harken::monitor::SetRecorderWriteFailureForTests(true);
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    std::array<unsigned char, 4096> buffer{};
    VnMonitorPollDesc poll{};
    poll.structSize = sizeof(poll);
    poll.buffer = buffer.data();
    poll.bufferBytes = buffer.size();
    VnMonitorPollResult pollResult{};
    pollResult.structSize = sizeof(pollResult);
    EXPECT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll, &pollResult));
    auto status = MakeStatus();
    const auto start = std::chrono::steady_clock::now();
    do {
        status = MakeStatus();
        ASSERT_EQ(VN_OK, Vn_MonitorGetRecordingStatus(handle, &status));
        if (status.state == VN_MONITOR_RECORDING_STATE_FAILED) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_FAILED, status.state);
    EXPECT_FALSE(harken::monitor::RecorderConsumerEnabledForTests());
    EXPECT_EQ(0u, status.queueBytes);
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(1u, status.recoveryNeeded);
    EXPECT_EQ(static_cast<std::uint32_t>(-VN_MONITOR_RECORDING_FAILED),
              status.errorReason);
    EXPECT_EQ(VN_MONITOR_RECORDING_FAILED,
              Vn_MonitorStopRecording(handle, lease.leaseGeneration));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    RemoveIfExists(path);
}

TEST(MonitorSession, RecordStopCaptureDoesNotStopFile) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::string path = TempSessionPath("stop-capture-keeps.vnmon");
    RemoveIfExists(path);

    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc openDesc = MakeRecordDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&openDesc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    auto desc = MakeRecordingDesc(path.c_str(), 8ull * 1024ull * 1024ull);
    ASSERT_EQ(VN_OK,
              Vn_MonitorStartRecording(handle, lease.leaseGeneration, &desc));
    VnMonitorEffectiveCapturePlan plan{};
    plan.structSize = sizeof(plan);
    EXPECT_EQ(VN_OK, Vn_MonitorStopCapture(handle, lease.leaseGeneration, &plan));
    auto status = MakeStatus();
    ASSERT_EQ(VN_OK, Vn_MonitorGetRecordingStatus(handle, &status));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_ACTIVE, status.state);
    status = StopAndWait(handle, lease.leaseGeneration);
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, status.state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    RemoveIfExists(path);
}
