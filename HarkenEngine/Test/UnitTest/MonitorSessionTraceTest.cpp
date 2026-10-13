#include "MonitorCaptureHubTestHooks.h"
#include "MonitorSessionFileWrite.h"
#include "Common/TestUtil.h"
#include "monitor_session_codec.h"

#include <harken/Harken.h>

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
    const auto dir = std::filesystem::temp_directory_path() / "harken-monitor-m44";
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

VnResult Acquire(VnMonitorSessionHandle handle, VnMonitorCaptureControlStatus* status) {
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
    return status;
}

std::string RecordAndStop(const char* name, bool withTrace) {
    const std::string path = TempSessionPath(name);
    RemoveIfExists(path);
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    EXPECT_EQ(VN_OK, engine.InitResult());
    std::string tracePath;
    if (withTrace) {
        tracePath = TempSessionPath("live.vtrc");
        RemoveIfExists(tracePath);
        EXPECT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
    }
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc openDesc = MakeRecordDesc();
    EXPECT_EQ(VN_OK, Vn_MonitorOpenSession(&openDesc, &handle));
    VnMonitorCaptureControlStatus lease{};
    EXPECT_EQ(VN_OK, Acquire(handle, &lease));
    auto desc = MakeRecordingDesc(path.c_str(), 8ull * 1024ull * 1024ull);
    EXPECT_EQ(VN_OK, Vn_MonitorStartRecording(handle, lease.leaseGeneration, &desc));
    std::array<unsigned char, 4096> buffer{};
    VnMonitorPollDesc poll{};
    poll.structSize = sizeof(poll);
    poll.buffer = buffer.data();
    poll.bufferBytes = buffer.size();
    VnMonitorPollResult pollResult{};
    pollResult.structSize = sizeof(pollResult);
    EXPECT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll, &pollResult));
    auto status = StopAndWait(handle, lease.leaseGeneration);
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, status.state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    if (withTrace) {
        EXPECT_EQ(VN_OK, Vn_TraceStop());
        RemoveIfExists(tracePath);
    }
    return path;
}

VnMonitorSessionScanResult ScanFile(const std::string& path,
                                    std::vector<std::uint8_t>* bytes,
                                    std::vector<VnMonitorSessionCommitView>* commits) {
    *bytes = ReadAll(path);
    commits->assign(16, VnMonitorSessionCommitView{});
    VnMonitorDecodedRecord scratch[16]{};
    std::uint8_t skip[4096]{};
    auto result = vn_monitor_scan_session(bytes->data(), bytes->size(), commits->data(),
                                          16, scratch, 16, skip, sizeof(skip));
    commits->resize(result.commitsWritten);
    return result;
}

} // namespace

TEST(MonitorSession, TraceAbsentKeepsFileLegalWithoutCorrelationChunk) {
    const std::string path = RecordAndStop("no-trace.vnmon", false);
    std::vector<std::uint8_t> bytes;
    std::vector<VnMonitorSessionCommitView> commits;
    const auto scan = ScanFile(path, &bytes, &commits);
    EXPECT_EQ(VN_OK, scan.status);
    EXPECT_GE(scan.commitsWritten, 1u);
    for (const auto& commit : commits) {
        EXPECT_NE(VN_MONITOR_CHUNK_TRACE_CORRELATION, commit.kind);
    }
    RemoveIfExists(path);
}

TEST(MonitorSession, TraceActiveWritesOptionalCorrelationChunk) {
    const std::string path = RecordAndStop("with-trace.vnmon", true);
    std::vector<std::uint8_t> bytes;
    std::vector<VnMonitorSessionCommitView> commits;
    const auto scan = ScanFile(path, &bytes, &commits);
    EXPECT_EQ(VN_OK, scan.status);
    bool found = false;
    for (const auto& commit : commits) {
        if (commit.kind != VN_MONITOR_CHUNK_TRACE_CORRELATION) {
            continue;
        }
        found = true;
        EXPECT_EQ(VN_MONITOR_CHUNK_OPTIONAL, commit.flags);
        EXPECT_EQ(harken::monitor::kTraceCorrelationPackedBytes, commit.payloadBytes);
        ASSERT_GE(commit.payloadBytes, 24u);
        EXPECT_EQ(1u, commit.payload[0]);
        EXPECT_EQ(0u, commit.payload[1]);
        EXPECT_EQ(0u, commit.payload[2]);
        EXPECT_EQ(0u, commit.payload[3]);
        std::uint64_t epoch = 0;
        std::memcpy(&epoch, commit.payload + 16, sizeof(epoch));
        EXPECT_NE(0u, epoch);
    }
    EXPECT_TRUE(found);
    RemoveIfExists(path);
}

TEST(MonitorSession, TraceOptionalWriteFailureDoesNotLeavePartialChunk) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::string path = TempSessionPath("optional-fail.vnmon");
    const std::string tracePath = TempSessionPath("optional-fail.vtrc");
    RemoveIfExists(path);
    RemoveIfExists(tracePath);
    ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc openDesc = MakeRecordDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&openDesc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    harken::monitor::SetRecorderOptionalWriteFailureForTests(true);
    auto desc = MakeRecordingDesc(path.c_str(), 8ull * 1024ull * 1024ull);
    EXPECT_EQ(VN_OK, Vn_MonitorStartRecording(handle, lease.leaseGeneration, &desc));
    std::array<unsigned char, 4096> buffer{};
    VnMonitorPollDesc poll{};
    poll.structSize = sizeof(poll);
    poll.buffer = buffer.data();
    poll.bufferBytes = buffer.size();
    VnMonitorPollResult pollResult{};
    pollResult.structSize = sizeof(pollResult);
    EXPECT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll, &pollResult));
    auto status = StopAndWait(handle, lease.leaseGeneration);
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, status.state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    EXPECT_EQ(VN_OK, Vn_TraceStop());
    std::vector<std::uint8_t> bytes;
    std::vector<VnMonitorSessionCommitView> commits;
    const auto scan = ScanFile(path, &bytes, &commits);
    EXPECT_EQ(VN_OK, scan.status);
    EXPECT_GE(scan.commitsWritten, 1u);
    bool foundCorrelation = false;
    bool foundRequired = false;
    for (const auto& commit : commits) {
        if (commit.kind == VN_MONITOR_CHUNK_TRACE_CORRELATION) {
            foundCorrelation = true;
        }
        if ((commit.flags & VN_MONITOR_CHUNK_REQUIRED) != 0) {
            foundRequired = true;
        }
    }
    EXPECT_FALSE(foundCorrelation);
    EXPECT_TRUE(foundRequired);
    RemoveIfExists(path);
    RemoveIfExists(tracePath);
}

TEST(MonitorSession, TraceOptionalRestoreFailureFailsRecording) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::string path = TempSessionPath("optional-restore-fail.vnmon");
    const std::string tracePath = TempSessionPath("optional-restore-fail.vtrc");
    RemoveIfExists(path);
    RemoveIfExists(tracePath);
    ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc openDesc = MakeRecordDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&openDesc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    harken::monitor::SetRecorderOptionalWriteFailureForTests(true);
    harken::monitor::SetRecorderOptionalRestoreFailureForTests(true);
    auto desc = MakeRecordingDesc(path.c_str(), 8ull * 1024ull * 1024ull);
    EXPECT_EQ(VN_MONITOR_RECORDING_FAILED,
              Vn_MonitorStartRecording(handle, lease.leaseGeneration, &desc));
    VnMonitorRecordingStatus status = MakeStatus();
    EXPECT_EQ(VN_OK, Vn_MonitorGetRecordingStatus(handle, &status));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_FAILED, status.state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    EXPECT_EQ(VN_OK, Vn_TraceStop());
    EXPECT_FALSE(std::filesystem::exists(path));
    RemoveIfExists(tracePath);
}

TEST(MonitorSession, TraceStartRecordingDoesNotStartOrStopTrace) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::string path = TempSessionPath("no-trace-api.vnmon");
    RemoveIfExists(path);
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc openDesc = MakeRecordDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&openDesc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    auto desc = MakeRecordingDesc(path.c_str(), 8ull * 1024ull * 1024ull);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(handle, lease.leaseGeneration, &desc));
    EXPECT_EQ(VN_OK, Vn_TraceStop());
    auto status = StopAndWait(handle, lease.leaseGeneration);
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, status.state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    RemoveIfExists(path);
}
