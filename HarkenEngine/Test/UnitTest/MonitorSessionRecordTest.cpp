#include <harken/Harken.h>
#include <harken/core/VoiceLedger.h>

#include "MonitorCaptureHubTestHooks.h"
#include "MonitorCaptureHub.h"
#include "MonitorDerivedDispatch.h"
#include "MonitorMeterHandlerTestHooks.h"
#include "MonitorRecordingCodec.h"
#include "MonitorSessionRecover.h"
#include "MonitorSessionFileWrite.h"
#include "Common/ObjectIdTestSupport.h"
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

namespace {
using namespace harken::monitor;

VnMonitorSessionHandle OpenTypedRecordingOwner(std::uint32_t streams, bool symbols = false) {
    VnMonitorSessionDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = streams;
    desc.requestedPermissionMask = VN_MONITOR_PERM_RECORD | VN_MONITOR_PERM_CAPTURE_CONTROL;
    if (streams & VN_MONITOR_STREAM_EVENT) desc.requestedPermissionMask |= VN_MONITOR_PERM_OBSERVE_EVENTS;
    if (streams & VN_MONITOR_STREAM_STATE) desc.requestedPermissionMask |= VN_MONITOR_PERM_OBSERVE_STATE;
    if (streams & VN_MONITOR_STREAM_TIME_SERIES) desc.requestedPermissionMask |= VN_MONITOR_PERM_OBSERVE_MEASUREMENTS;
    if (streams & VN_MONITOR_STREAM_OPTIONAL_AUDIO)
        desc.requestedPermissionMask |= VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_RAW_AUDIO;
    if (symbols) desc.requestedPermissionMask |= VN_MONITOR_PERM_SYMBOLS;
    desc.requestSymbols = symbols ? 1u : 0u;
    desc.requestRawAudio = (streams & VN_MONITOR_STREAM_OPTIONAL_AUDIO) != 0 ? 1u : 0u;
    desc.consumerQueueBytes = 65536;
    desc.requestedExecutionFactVersion = 1;
    VnMonitorSessionHandle handle = 0;
    EXPECT_EQ(VN_OK, Vn_MonitorOpenSession(reinterpret_cast<const VnMonitorSessionDesc*>(&desc), &handle));
    StopDrainForTests();
    return handle;
}

VnMonitorRecordingStatus PumpTypedRecording(VnMonitorSessionHandle handle) {
    for (unsigned i = 0; i != 32; ++i) {
        DrainOnceForTests();
        PumpRecorderForTests();
    }
    auto status = MakeStatus();
    EXPECT_EQ(VN_OK, Vn_MonitorGetRecordingStatus(handle, &status));
    return status;
}

struct TypedFile {
    RecordingDescriptor descriptor{};
    RecordingEnd end{};
    std::vector<VnMonitorRecordHeader> records;
    std::vector<RecordingSource> sources;
    std::vector<std::uint64_t> tickets;
};

TypedFile ReadTypedFile(const std::string& path, bool complete = true) {
    const auto bytes = ReadAll(path);
    const auto scan = ScanSessionBytes(bytes.data(), bytes.size());
    EXPECT_EQ(VN_OK, scan.status);
    EXPECT_EQ(complete ? 0u : 1u, scan.recoveryNeeded);
    TypedFile file;
    unsigned descriptors = 0, ends = 0;
    for (const auto& chunk : scan.commits) {
        if (chunk.kind == VN_MONITOR_CHUNK_EXECUTION_DESCRIPTOR) {
            EXPECT_TRUE(DecodeRecordingDescriptor(chunk.payload.data(), chunk.payload.size(), file.descriptor));
            ++descriptors;
        } else if (chunk.kind == VN_MONITOR_CHUNK_EXECUTION_RECORD) {
            EXPECT_GE(chunk.payload.size(), kRecordingPositionBytes + kPackedEnvelopeBytes);
            VnMonitorRecordHeader header{};
            EXPECT_TRUE(ReadIdentityHeader(chunk.payload.data() + kRecordingPositionBytes,
                chunk.payload.size() - kRecordingPositionBytes, header));
            file.records.push_back(header);
            ExecutionPayloadReader read{chunk.payload.data()};
            std::uint32_t source = 0, reserved = 0;
            std::uint64_t ticket = 0;
            read(source); read(reserved); read(ticket);
            file.sources.push_back(static_cast<RecordingSource>(source));
            file.tickets.push_back(ticket);
        } else if (chunk.kind == VN_MONITOR_CHUNK_EXECUTION_END) {
            EXPECT_TRUE(DecodeRecordingEnd(chunk.payload.data(), chunk.payload.size(), file.end));
            ++ends;
        }
    }
    EXPECT_EQ(1u, descriptors);
    EXPECT_EQ(1u, ends);
    EXPECT_EQ(file.records.size(), file.end.recordCount);
    return file;
}
}

TEST(MonitorTypedRecording, StateOnlyUsesPrivateBoundedReceiverAndFinishesItsBaseline) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_STATE);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("typed-state.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    const auto receiver = RecordingReceiverForTests();
    ASSERT_NE(0u, receiver);
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorCloseSession(receiver));
    VnMonitorCaptureControlStatus guessed{};
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Acquire(receiver, &guessed));
    EXPECT_FALSE(ExecutionObservationEnabled());
    EXPECT_FALSE(EventCatalogEnabled());
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    PumpRecorderForTests();
    auto status = MakeStatus();
    ASSERT_EQ(VN_OK, Vn_MonitorGetRecordingStatus(owner, &status));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPING, status.state);
    status = PumpTypedRecording(owner);
    ASSERT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, status.state);
    EXPECT_EQ(0u, status.recoveryNeeded);
    const auto file = ReadTypedFile(path);
    EXPECT_EQ(VN_MONITOR_STREAM_STATE, file.descriptor.streams);
    EXPECT_NE(file.descriptor.ownerEpoch, file.descriptor.epoch);
    EXPECT_NE(file.descriptor.recordingEpoch, file.descriptor.epoch);
    EXPECT_GT(file.records.size(), 2u);
    for (const auto& record : file.records) {
        EXPECT_EQ(VN_MONITOR_STREAM_STATE, record.streamKind);
        EXPECT_EQ(file.descriptor.epoch, record.monitorSessionEpoch);
    }
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, OwnerPollCannotStealRecordsAndRawWindowsExcludeEarlierAndLaterSlots) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_TIME_SERIES);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    ASSERT_TRUE(PublishSyntheticTimeSeriesForTests(1));
    const auto path = TempSessionPath("typed-window.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_TRUE(PublishSyntheticTimeSeriesForTests(2));
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    ASSERT_TRUE(PublishSyntheticTimeSeriesForTests(1));
    DrainOnceForTests();
    std::array<std::uint8_t, 65536> buffer{};
    VnMonitorPollDesc poll{sizeof(poll), 0, buffer.data(), buffer.size()};
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(owner, &poll, &result));
    EXPECT_GT(result.recordsWritten, 0u);
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(owner).state);
    const auto file = ReadTypedFile(path);
    unsigned facts = 0, measurements = 0;
    for (const auto& record : file.records) {
        facts += record.recordKind == VN_MONITOR_RECORD_EXECUTION_FACT;
        measurements += record.recordKind == VN_MONITOR_RECORD_TIME_SERIES;
        EXPECT_NE(VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL, record.recordKind);
    }
    EXPECT_EQ(1u, facts);
    EXPECT_EQ(2u, measurements);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, DerivedMeasurementsRetainTheirParentTicketsAndControlActivation) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_OPTIONAL_AUDIO);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("typed-derived.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));

    VnMonitorTapDescriptor tap{};
    std::uint32_t registryBytes = 0;
    std::uint64_t registryRevision = 0;
    ASSERT_EQ(VN_MONITOR_BUFFER_TOO_SMALL,
        Vn_MonitorCaptureRegistry(VN_MONITOR_REGISTRY_TAP, nullptr, 0, &registryBytes, &registryRevision));
    ASSERT_EQ(0u, registryBytes % sizeof(tap));
    std::vector<VnMonitorTapDescriptor> taps(registryBytes / sizeof(tap));
    ASSERT_EQ(VN_OK, Vn_MonitorCaptureRegistry(VN_MONITOR_REGISTRY_TAP, taps.data(), registryBytes,
        &registryBytes, &registryRevision));
    for (const auto& candidate : taps)
        if (candidate.tapKind == VN_MONITOR_TAP_MASTER_OUTPUT && candidate.ownerStableId == VN_BUS_MASTER)
            tap = candidate;
    ASSERT_NE(0u, tap.tapDescriptorId);
    ASSERT_NE(0u, tap.tapDescriptorGeneration);
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    std::array<VnMonitorMeasurementRequest, 2> requests{};
    for (auto& request : requests) {
        request.structSize = sizeof(request);
        request.channelMode = VN_MONITOR_CHANNEL_PER_CHANNEL;
        request.downsample = 1;
    }
    requests[0].kind = VN_MONITOR_MEASURE_WAVEFORM;
    requests[0].bucketFrames = 4;
    requests[1].kind = VN_MONITOR_MEASURE_SAMPLE_PEAK;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.taps = &tap;
    plan.measurementCount = static_cast<std::uint32_t>(requests.size());
    plan.measurements = requests.data();
    plan.downsample = 1;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(owner, lease.leaseGeneration, &plan, &accepted));
    AdvanceExecuteFrameForTests(1);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(owner, 0, &accepted));
    ASSERT_EQ(VN_MONITOR_PLAN_ACTIVE, accepted.activationState);
    const std::array<float, 6> samples{0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f};
    std::array<std::uint8_t, 1024> packed{};
    auto bytes = PackAudioChunkRecord(packed.data(), packed.size(), 1, accepted.capturePlanGeneration, 1,
        tap.tapDescriptorId, tap.tapDescriptorGeneration, 0, 48000, 1, 0, 0, 6, samples.data(), 6, 1);
    ASSERT_NE(0u, bytes);
    ASSERT_TRUE(PublishConformancePackedForTests(packed.data(), bytes));
    DrainOnceForTests();
    bytes = PackAudioChunkRecord(packed.data(), packed.size(), 2, accepted.capturePlanGeneration, 1,
        tap.tapDescriptorId, tap.tapDescriptorGeneration, 0, 48000, 1, 0, 8, 12, samples.data(), 4, 2);
    ASSERT_NE(0u, bytes);
    ASSERT_TRUE(PublishConformancePackedForTests(packed.data(), bytes));
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    ASSERT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(owner).state);
    const auto file = ReadTypedFile(path);
    std::uint64_t parents[2]{};
    unsigned raw = 0, waveform = 0, partial = 0, meter = 0, control = 0;
    for (std::size_t i = 0; i < file.records.size(); ++i) {
        const auto& record = file.records[i];
        if (record.recordKind == VN_MONITOR_RECORD_CAPTURE_PLAN_ACTIVATED) {
            ++control;
            EXPECT_EQ(RecordingSource::Control, file.sources[i]);
            EXPECT_EQ(file.descriptor.controlStart + 1, file.tickets[i]);
            continue;
        }
        ASSERT_EQ(RecordingSource::AudioRing, file.sources[i]);
        if (record.recordKind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
            ASSERT_LT(raw, 2u);
            parents[raw++] = file.tickets[i];
        } else {
            ASSERT_NE(0u, raw);
            EXPECT_EQ(parents[raw - 1], file.tickets[i]);
            waveform += record.recordKind == VN_MONITOR_RECORD_WAVEFORM_BUCKET;
            partial += record.recordKind == VN_MONITOR_RECORD_PARTIAL_BUCKET_DISCARDED;
            meter += record.recordKind == VN_MONITOR_RECORD_METER;
        }
    }
    EXPECT_EQ(2u, raw);
    EXPECT_EQ(2u, waveform);
    EXPECT_EQ(1u, partial);
    EXPECT_EQ(2u, meter);
    EXPECT_EQ(1u, control);
    EXPECT_EQ(parents[0] + 1, parents[1]);
    EXPECT_EQ(0u, file.end.controlDrops);
    EXPECT_EQ(file.descriptor.controlStart + 1, file.end.controlEnd);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, LostControlActivationCannotProduceACompleteFile) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_TIME_SERIES);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("typed-control-loss.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    plan.cadenceHz = 30;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    DropNextActivationRecordForTests(true);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(owner, lease.leaseGeneration, &plan, &accepted));
    AdvanceExecuteFrameForTests(1);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(owner, 0, &accepted));
    ASSERT_EQ(VN_MONITOR_PLAN_ACTIVE, accepted.activationState);
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    ASSERT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(owner).state);
    const auto file = ReadTypedFile(path, false);
    EXPECT_EQ(file.descriptor.controlStart + 1, file.end.controlEnd);
    EXPECT_EQ(1u, file.end.controlDrops);
    for (const auto& record : file.records)
        EXPECT_NE(VN_MONITOR_RECORD_CAPTURE_PLAN_ACTIVATED, record.recordKind);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, NativeLogTranscriptionAfterStopRetainsOriginalSourceAndFilteredCounts) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("typed-logs.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    ASSERT_TRUE(engine.Engine()->logicLogRing.Push({VN_LOG_WARN, harken::kLogCodeHotReloadRefused, 1, 2}));
    ASSERT_TRUE(engine.Engine()->logicLogRing.Push({VN_LOG_INFO, harken::kLogCodeHotReloadApplied, 3, 4}));
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPING, PumpTypedRecording(owner).state);
    ASSERT_TRUE(engine.Engine()->logicLogRing.Push({VN_LOG_WARN, harken::kLogCodeHotReloadRefused, 5, 6}));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(owner).state);
    const auto file = ReadTypedFile(path);
    ASSERT_EQ(1u, file.records.size());
    EXPECT_EQ(RecordingSource::LogicLog, file.sources[0]);
    EXPECT_EQ(VN_MONITOR_RECORD_EVENT_DETAIL, file.records[0].recordKind);
    EXPECT_EQ(1u, file.end.logFiltered[1]);
    EXPECT_EQ(2u, file.end.logEnds[1] - file.descriptor.logStarts[1]);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, NativeLogOverflowRemainsIncompleteAfterFilteredTranscription) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("typed-log-overflow.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    constexpr auto count = harken::LogRing::kCapacity + 3;
    for (unsigned i = 0; i < count; ++i)
        ASSERT_TRUE(engine.Engine()->logicLogRing.Push({VN_LOG_INFO, harken::kLogCodeHotReloadApplied, 1, i}));
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(owner).state);
    const auto file = ReadTypedFile(path, false);
    EXPECT_EQ(count, file.end.logEnds[1] - file.descriptor.logStarts[1]);
    EXPECT_EQ(harken::LogRing::kCapacity - 1, file.end.logFiltered[1]);
    EXPECT_EQ(4u, file.end.nativeLogDrops[1]);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, RawSourceOverflowCannotBeHiddenBySuccessfulStop) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_TIME_SERIES);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("typed-source-overflow.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    const auto capacity = ProducerRingCapacityForTests();
    EXPECT_FALSE(PublishSyntheticTimeSeriesForTests(capacity + 3));
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    ASSERT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(owner).state);
    const auto file = ReadTypedFile(path, false);
    EXPECT_EQ(3u, file.end.rawDrops[1]);
    EXPECT_EQ(capacity, file.end.ringEnds[1] - file.descriptor.ringStarts[1]);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, SelectionPoolLossIsFencedBeforeStopAndCannotProduceACompleteFile) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    harken::VoiceLedger ledger(1, 1, 1);
    harken::VoiceLedger::MonitorSelectionBacking monitorBacking;
    ASSERT_TRUE(ledger.PrepareMonitorSelectionBacking(
        static_cast<std::uint32_t>(ledger.MonitorSelectionCapacity()), &monitorBacking));
    VnVoiceHandle voice = 0;
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.Acquire(&voice, &slot));
    const auto render = UINT64_C(1) << 32;
    ledger.BindRender(slot, render, 128);
    harken::VoiceLedger::MonitorSelection retained, overflow;
    ASSERT_TRUE(ledger.CaptureMonitorSelection(slot, render, &retained));
    const auto path = TempSessionPath("typed-selection-pool-loss.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    EXPECT_FALSE(ledger.CaptureMonitorSelection(slot, render, &overflow));
    auto branch = retained;
    harken::MonitorControlCause cause{};
    cause.executionOrdinal = 3;
    branch.SetCause(cause);
    EXPECT_EQ(0u, branch.RenderActivation());
    EXPECT_EQ(render, retained.RenderActivation());
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    PumpRecorderForTests();
    auto status = MakeStatus();
    ASSERT_EQ(VN_OK, Vn_MonitorGetRecordingStatus(owner, &status));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPING, status.state);
    // This later fact cannot replace either gap inside the frozen window.
    ledger.PublishMonitorSelection(retained, &cause);
    status = PumpTypedRecording(owner);
    ASSERT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, status.state) << status.errorReason;
    EXPECT_EQ(1u, status.recoveryNeeded);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live, ledger.Classify(voice).kind);
    EXPECT_EQ(render, ledger.Classify(voice).renderHandle);
    const auto file = ReadTypedFile(path, false);
    EXPECT_EQ(2u, file.end.ends[0] - file.descriptor.starts[0]);
    EXPECT_GE(file.end.completed[0], file.end.ends[0]);
    EXPECT_EQ(2u, file.end.sourceDrops[1]);
    EXPECT_EQ(2u, file.end.eventSourceDrops);
    EXPECT_EQ(1u, file.end.boundaryComplete);
    for (const auto& record : file.records)
        EXPECT_NE(VN_MONITOR_RECORD_EXECUTION_FACT, record.recordKind);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, FileBudgetFailurePreservesAnIncompleteCommittedPrefix) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_TIME_SERIES);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("typed-file-budget.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 2048);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    ASSERT_TRUE(PublishSyntheticTimeSeriesForTests(8));
    const auto status = PumpTypedRecording(owner);
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_FAILED, status.state);
    EXPECT_EQ(static_cast<std::uint32_t>(-VN_MONITOR_BUDGET_EXCEEDED), status.errorReason);
    EXPECT_EQ(1u, status.recoveryNeeded);
    EXPECT_FALSE(RecorderConsumerEnabledForTests());
    const auto bytes = ReadAll(path);
    EXPECT_LE(bytes.size(), desc.fileByteBudget);
    const auto scan = ScanSessionBytes(bytes.data(), bytes.size());
    EXPECT_EQ(VN_OK, scan.status);
    EXPECT_EQ(1u, scan.recoveryNeeded);
    ASSERT_FALSE(scan.commits.empty());
    EXPECT_EQ(VN_MONITOR_CHUNK_EXECUTION_DESCRIPTOR, scan.commits.front().kind);
    for (const auto& chunk : scan.commits) EXPECT_NE(VN_MONITOR_CHUNK_EXECUTION_END, chunk.kind);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, TeardownWithUntranscribedSourcesCannotProduceACompleteFile) {
    const auto path = TempSessionPath("typed-teardown.vnmon");
    {
        harkentest::EngineSession engine(harkentest::MakeNullConfig());
        ASSERT_EQ(VN_OK, engine.InitResult());
        ASSERT_EQ(VN_OK, Vn_Update());
        const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT);
        VnMonitorCaptureControlStatus lease{};
        ASSERT_EQ(VN_OK, Acquire(owner, &lease));
        auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
        ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
        ASSERT_TRUE(engine.Engine()->logicLogRing.Push({VN_LOG_WARN, harken::kLogCodeHotReloadRefused, 1, 2}));
    }
    const auto bytes = ReadAll(path);
    const auto scan = ScanSessionBytes(bytes.data(), bytes.size());
    EXPECT_EQ(VN_OK, scan.status);
    EXPECT_EQ(1u, scan.recoveryNeeded);
    ASSERT_FALSE(scan.commits.empty());
    EXPECT_EQ(VN_MONITOR_CHUNK_EXECUTION_DESCRIPTOR, scan.commits.front().kind);
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, SameOwnerReacquisitionStopsOriginalLeaseAndDoesNotInheritItsFile) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto original = lease.leaseGeneration;
    const auto path = TempSessionPath("typed-lease.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, original, &desc));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    ASSERT_NE(original, lease.leaseGeneration);
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    EXPECT_EQ(VN_MONITOR_STALE_LEASE, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(owner).state);
    const auto file = ReadTypedFile(path);
    EXPECT_EQ(original, file.descriptor.leaseGeneration);
    EXPECT_EQ(1u, file.records.size());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, DrainThreadExpiryWithoutUpdateEndsIncompleteAndDefersLogicCleanup) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("typed-expiry.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    ASSERT_TRUE(engine.Engine()->logicLogRing.Push({VN_LOG_WARN, harken::kLogCodeHotReloadRefused, 1, 2}));
    SetLeaseExpiryExecuteFrameForTests(ExecuteFrameForTests() + 1);
    std::thread drain([] { AdvanceExecuteFrameForTests(1); DrainOnceForTests(); PumpRecorderForTests(); });
    drain.join();
    auto status = MakeStatus();
    ASSERT_EQ(VN_OK, Vn_MonitorGetRecordingStatus(owner, &status));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, status.state);
    EXPECT_EQ(1u, status.recoveryNeeded);
    const auto file = ReadTypedFile(path, false);
    EXPECT_EQ(0u, file.end.boundaryComplete);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    EXPECT_FALSE(ExecutionObservationEnabled());
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, QueueLossSurvivesSummaryFailureAndProducesValidIncompleteFile) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_STATE);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("typed-overflow.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    SetRecordingQueueBudgetForTests(1);
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    const auto status = PumpTypedRecording(owner);
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, status.state);
    EXPECT_EQ(1u, status.recoveryNeeded);
    const auto file = ReadTypedFile(path, false);
    EXPECT_GT(file.end.deliveryDrops[4], 0u);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, BroaderSymbolSessionCannotEnlargeARecordersGrant) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnMonitorHostSessionV1 host{};
    host.structSize = sizeof(host);
    ASSERT_EQ(VN_OK, Vn_MonitorBeginHostSession(&host));
    VnMonitorHostRegistrationV2 hostObject{};
    hostObject.structSize = sizeof(hostObject);
    hostObject.engineInstance = host.engineInstance;
    hostObject.sdkSession = host.sdkSession;
    hostObject.emitter = emitter;
    hostObject.ownershipKind = VN_MONITOR_HOST_UNITY_EMITTER;
    hostObject.localSequence = 1;
    hostObject.registrationRevision = 1;
    hostObject.nameBytes = 4;
    std::memcpy(hostObject.name, "Host", 4);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&hostObject));
    const auto broad = OpenTypedRecordingOwner(VN_MONITOR_STREAM_STATE, true);
    const auto narrow = OpenTypedRecordingOwner(VN_MONITOR_STREAM_STATE);
    for (const auto owner : {narrow, broad}) {
        VnMonitorCaptureControlStatus lease{};
        ASSERT_EQ(VN_OK, Acquire(owner, &lease));
        const auto path = TempSessionPath(owner == broad ? "typed-symbols.vnmon" : "typed-no-symbols.vnmon");
        auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
        ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
        ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
        ASSERT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(owner).state);
        const auto file = ReadTypedFile(path);
        unsigned symbols = 0;
        for (const auto& record : file.records) symbols += record.recordKind == VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL;
        EXPECT_EQ(owner == broad ? 3u : 0u, symbols);
        EXPECT_EQ(VN_OK, Vn_MonitorReleaseCaptureControl(owner, lease.leaseGeneration));
        RemoveIfExists(path);
    }
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(narrow));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(broad));
}

TEST(MonitorTypedRecording, OwnerCloseAndSlotReuseCannotAttachReplacementTraffic) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("typed-owner-close.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    const auto replacement = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT);
    ASSERT_NE(owner, replacement);
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    ASSERT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(replacement).state);
    const auto file = ReadTypedFile(path);
    EXPECT_EQ(owner, file.descriptor.owner);
    EXPECT_EQ(1u, file.records.size());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(replacement));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, FiniteWriterBatchContinuesWithoutAnotherProducerWake) {
    auto config = harkentest::MakeNullConfig();
    config.maxEmitters = 256;
    harkentest::EngineSession engine(config);
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_STATE);
    VnEmitterHandle emitter = 0;
    for (unsigned i = 0; i != 150; ++i) ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("typed-batch.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    DrainOnceForTests();
    StartDrainForTests();
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, StopAndWait(owner, lease.leaseGeneration).state);
    const auto file = ReadTypedFile(path);
    EXPECT_GT(file.records.size(), 128u);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, FailedWriterCleanupCannotDetachAnImmediatelyRestartedQueue) {
    for (const bool typed : {false, true}) {
        SCOPED_TRACE(typed);
        harkentest::EngineSession engine(harkentest::MakeNullConfig());
        ASSERT_EQ(VN_OK, engine.InitResult());
        ASSERT_EQ(VN_OK, Vn_Update());
        VnMonitorSessionHandle owner = 0;
        if (typed) owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_TIME_SERIES);
        else {
            auto open = MakeRecordDesc();
            ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&open, &owner));
            StopDrainForTests();
        }
        VnMonitorCaptureControlStatus lease{};
        ASSERT_EQ(VN_OK, Acquire(owner, &lease));
        const auto failedPath = TempSessionPath("failed-restart.vnmon");
        const auto newPath = TempSessionPath("new-restart.vnmon");
        auto failedDesc = MakeRecordingDesc(failedPath.c_str(), 1024 * 1024);
        auto newDesc = MakeRecordingDesc(newPath.c_str(), 1024 * 1024);
        ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &failedDesc));
        ASSERT_TRUE(PublishSyntheticTimeSeriesForTests(1));
        DrainOnceForTests();
        struct Restart {
            VnMonitorSessionHandle owner;
            std::uint64_t lease;
            VnMonitorRecordingDesc* desc;
            bool invoked = false;
        } restart{owner, lease.leaseGeneration, &newDesc};
        SetBeforeRecorderFailureCleanupForTests([](void* raw) {
            auto& context = *static_cast<Restart*>(raw);
            context.invoked = true;
            ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(context.owner, context.lease, context.desc));
            ASSERT_TRUE(PublishSyntheticTimeSeriesForTests(1));
            DrainOnceForTests();
        }, &restart);
        SetRecorderWriteFailureForTests(true);
        PumpRecorderForTests();
        ASSERT_TRUE(restart.invoked);
        ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
        EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(owner).state);
        if (typed) {
            const auto file = ReadTypedFile(newPath);
            ASSERT_EQ(1u, file.records.size());
            EXPECT_EQ(VN_MONITOR_RECORD_TIME_SERIES, file.records[0].recordKind);
        } else {
            const auto bytes = ReadAll(newPath);
            const auto scan = ScanSessionBytes(bytes.data(), bytes.size());
            EXPECT_EQ(VN_OK, scan.status);
            bool found = false;
            for (const auto& chunk : scan.commits) found |= chunk.kind == VN_MONITOR_CHUNK_EVENTS;
            EXPECT_TRUE(found);
        }
        EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
        RemoveIfExists(failedPath);
        RemoveIfExists(newPath);
    }
}

TEST(MonitorTypedRecording, ExpiryAfterExplicitStopPreservesWindowAndFinishesIncompleteWithoutUpdate) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("stop-then-expire.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    ASSERT_TRUE(engine.Engine()->logicLogRing.Push({VN_LOG_WARN, harken::kLogCodeHotReloadRefused, 1, 2}));
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPING, PumpTypedRecording(owner).state);
    ASSERT_TRUE(engine.Engine()->logicLogRing.Push({VN_LOG_WARN, harken::kLogCodeHotReloadRefused, 3, 4}));
    SetLeaseExpiryExecuteFrameForTests(ExecuteFrameForTests() + 1);
    std::thread drain([] { AdvanceExecuteFrameForTests(1); DrainOnceForTests(); PumpRecorderForTests(); });
    drain.join();
    auto status = MakeStatus();
    ASSERT_EQ(VN_OK, Vn_MonitorGetRecordingStatus(owner, &status));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, status.state);
    const auto file = ReadTypedFile(path, false);
    EXPECT_EQ(1u, file.end.logEnds[1] - file.descriptor.logStarts[1]);
    EXPECT_EQ(0u, file.end.boundaryComplete);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, LossAfterReceiverOpensCannotBecomeAPreRecordingBaseline) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_TIME_SERIES);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    ASSERT_TRUE(PublishSyntheticTimeSeriesForTests(ProducerRingCapacityForTests()));
    SetAfterRecordingReceiverOpenForTests([](void*) {
        EXPECT_FALSE(PublishSyntheticTimeSeriesForTests(1));
    }, nullptr);
    const auto path = TempSessionPath("start-window-loss.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(owner).state);
    const auto file = ReadTypedFile(path, false);
    EXPECT_EQ(0u, file.descriptor.rawDropStarts[1]);
    EXPECT_EQ(1u, file.end.rawDrops[1]);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, PoppedSourceMustFinishProjectionBeforeStopCanFinish) {
    for (const bool execution : {false, true}) {
        SCOPED_TRACE(execution);
        harkentest::EngineSession engine(harkentest::MakeNullConfig());
        ASSERT_EQ(VN_OK, engine.InitResult());
        ASSERT_EQ(VN_OK, Vn_Update());
        const auto owner = OpenTypedRecordingOwner(execution ? VN_MONITOR_STREAM_EVENT : VN_MONITOR_STREAM_TIME_SERIES);
        VnMonitorCaptureControlStatus lease{};
        ASSERT_EQ(VN_OK, Acquire(owner, &lease));
        const auto path = TempSessionPath("popped-not-projected.vnmon");
        auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
        ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
        struct Hold {
            Hold() { ResetDrainEnteredForTests(); SetDrainHoldForTests(true); }
            ~Hold() { SetDrainHoldForTests(false); }
        } hold;
        if (execution) {
            VnEmitterHandle emitter = 0;
            ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        } else ASSERT_TRUE(PublishSyntheticTimeSeriesForTests(1));
        StartDrainForTests();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (DrainEnteredForTests() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        ASSERT_GT(DrainEnteredForTests(), 0u);
        ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
        PumpRecorderForTests();
        auto status = MakeStatus();
        ASSERT_EQ(VN_OK, Vn_MonitorGetRecordingStatus(owner, &status));
        EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPING, status.state);
        SetDrainHoldForTests(false);
        EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, StopAndWait(owner, lease.leaseGeneration).state);
        const auto file = ReadTypedFile(path);
        EXPECT_EQ(1u, file.records.size());
        EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
        RemoveIfExists(path);
    }
}

TEST(MonitorTypedRecording, FirstCommittedChunkIdentifiesAnIncompleteTypedFile) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_TIME_SERIES);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("first-typed-chunk.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(owner).state);
    const auto bytes = ReadAll(path);
    const auto complete = ScanSessionBytes(bytes.data(), bytes.size());
    ASSERT_EQ(VN_OK, complete.status);
    ASSERT_FALSE(complete.commits.empty());
    EXPECT_EQ(VN_MONITOR_CHUNK_EXECUTION_DESCRIPTOR, complete.commits.front().kind);
    const auto firstBytes = kSessionFileHeaderPackedBytes + complete.commits.front().rawBytes.size();
    const auto truncated = ScanSessionBytes(bytes.data(), firstBytes);
    EXPECT_EQ(VN_OK, truncated.status);
    EXPECT_EQ(1u, truncated.recoveryNeeded);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

TEST(MonitorTypedRecording, StopInsideIdentityTransactionWaitsForItsOuterBoundary) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto owner = OpenTypedRecordingOwner(VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE);
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    const auto path = TempSessionPath("deferred-stop.vnmon");
    auto desc = MakeRecordingDesc(path.c_str(), 1024 * 1024);
    ASSERT_EQ(VN_OK, Vn_MonitorStartRecording(owner, lease.leaseGeneration, &desc));
    {
        IdentityMutationScope outer;
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        ASSERT_EQ(VN_OK, Vn_MonitorStopRecording(owner, lease.leaseGeneration));
        EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPING, PumpTypedRecording(owner).state);
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    }
    EXPECT_EQ(VN_MONITOR_RECORDING_STATE_STOPPED, PumpTypedRecording(owner).state);
    const auto file = ReadTypedFile(path);
    unsigned executionFacts = 0;
    for (const auto& record : file.records) executionFacts += record.recordKind == VN_MONITOR_RECORD_EXECUTION_FACT;
    EXPECT_EQ(2u, executionFacts);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    RemoveIfExists(path);
}

namespace {
std::vector<std::uint8_t> CodecFile(RecordingDescriptor descriptor, RecordingEnd end,
    const std::vector<std::vector<std::uint8_t>>& records, bool withDescriptor = true, bool withEnd = true) {
    std::vector<std::uint8_t> file(kSessionFileHeaderPackedBytes), chunk;
    PackSessionFileHeader(file.data(), 1, descriptor.engine, 0, 0);
    std::uint64_t sequence = 1;
    const auto append = [&](std::uint16_t kind, const std::uint8_t* bytes, std::size_t size) {
        PackSessionChunk(&chunk, kind, VN_MONITOR_CHUNK_REQUIRED, sequence++, bytes, size);
        file.insert(file.end(), chunk.begin(), chunk.end());
    };
    if (withDescriptor) {
        std::uint8_t bytes[kRecordingDescriptorBytes]{};
        ExecutionPayloadWriter write{bytes};
        VisitRecordingDescriptor(write, descriptor);
        EXPECT_EQ(sizeof(bytes), write.offset);
        append(VN_MONITOR_CHUNK_EXECUTION_DESCRIPTOR, bytes, sizeof(bytes));
    }
    for (const auto& record : records) append(VN_MONITOR_CHUNK_EXECUTION_RECORD, record.data(), record.size());
    if (withEnd) {
        std::uint8_t bytes[kRecordingEndBytes]{};
        ExecutionPayloadWriter write{bytes};
        VisitRecordingEnd(write, end);
        EXPECT_EQ(sizeof(bytes), write.offset);
        append(VN_MONITOR_CHUNK_EXECUTION_END, bytes, sizeof(bytes));
    }
    return file;
}

struct CodecCandidate {
    RecordingDescriptor descriptor{};
    RecordingEnd end{};
    std::vector<std::uint8_t> record;
    CodecCandidate() {
        descriptor.owner = 256;
        descriptor.ownerGeneration = 1;
        descriptor.ownerEpoch = 2;
        descriptor.leaseGeneration = 3;
        descriptor.engine = 4;
        descriptor.epoch = 5;
        descriptor.recordingEpoch = 6;
        descriptor.startedNs = 7;
        descriptor.streams = VN_MONITOR_STREAM_TIME_SERIES;
        descriptor.permissions = VN_MONITOR_PERM_RECORD | VN_MONITOR_PERM_OBSERVE_MEASUREMENTS;
        record.resize(kRecordingPositionBytes + PackedTimeSeriesBytes());
        PackRecordingPosition(record.data(), {RecordingSource::AudioRing, 0, 1, PackedTimeSeriesBytes()});
        auto* packed = record.data() + kRecordingPositionBytes;
        PackSyntheticTimeSeries(packed, PackedTimeSeriesBytes(), 1);
        WriteU64(packed + 48, 1);
        WriteU64(packed + 64, descriptor.epoch);
        end.engine = descriptor.engine;
        end.epoch = descriptor.epoch;
        end.recordingEpoch = descriptor.recordingEpoch;
        end.stoppedNs = 8;
        end.ringEnds[1] = end.ringCompleted[1] = 1;
        end.boundaryComplete = 1;
        end.recordCount = 1;
        end.checksum = IdentityChecksum(kIdentityChecksumSeed, record.data(), static_cast<std::uint32_t>(record.size()));
    }
    SessionRecoverResult Scan(bool descriptorPresent = true, bool endPresent = true) const {
        const auto bytes = CodecFile(descriptor, end, {record}, descriptorPresent, endPresent);
        return ScanSessionBytes(bytes.data(), bytes.size());
    }
};
}

TEST(MonitorTypedRecordingCodec, ValidRequiredFramingAndMissingEndHaveDistinctCompleteness) {
    CodecCandidate candidate;
    EXPECT_EQ(VN_OK, candidate.Scan().status);
    EXPECT_EQ(0u, candidate.Scan().recoveryNeeded);
    EXPECT_EQ(VN_OK, candidate.Scan(true, false).status);
    EXPECT_EQ(1u, candidate.Scan(true, false).recoveryNeeded);
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, candidate.Scan(false).status);
    candidate.end.deliveryDrops[0] = 1;
    EXPECT_EQ(VN_OK, candidate.Scan().status);
    EXPECT_EQ(1u, candidate.Scan().recoveryNeeded);
}

TEST(MonitorTypedRecordingCodec, RejectsMalformedProvenanceAndMismatchedEndEvenWithValidChunkCrc) {
    const CodecCandidate good;
    auto candidate = good;
    candidate.end.checksum ^= 1;
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, candidate.Scan().status);
    candidate = good;
    candidate.end.recordingEpoch += 1;
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, candidate.Scan().status);
    candidate = good;
    WriteU64(candidate.record.data() + 8, 0);
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, candidate.Scan().status);
    candidate = good;
    candidate.end.ringEnds[1] = 0;
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, candidate.Scan().status);
    candidate = good;
    candidate.descriptor.streams = VN_MONITOR_STREAM_EVENT;
    candidate.descriptor.permissions = VN_MONITOR_PERM_RECORD | VN_MONITOR_PERM_OBSERVE_EVENTS;
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, candidate.Scan().status);
}

TEST(MonitorTypedRecordingCodec, RejectsUnknownRequiredKindsVersionsPayloadsAndPaddingWithRecomputedChecksums) {
    const CodecCandidate good;
    const auto rehash = [](CodecCandidate& candidate) {
        candidate.end.checksum = IdentityChecksum(kIdentityChecksumSeed, candidate.record.data(),
            static_cast<std::uint32_t>(candidate.record.size()));
    };
    for (const auto kind : {0u, 22u, 1000u}) {
        auto candidate = good;
        WriteU32(candidate.record.data() + kRecordingPositionBytes, kind);
        WriteU32(candidate.record.data() + kRecordingPositionBytes + 8, VN_MONITOR_RECORD_FLAG_REQUIRED);
        rehash(candidate);
        EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, candidate.Scan().status) << kind;
    }
    auto candidate = good;
    WriteU32(candidate.record.data() + kRecordingPositionBytes + 4, 2);
    rehash(candidate);
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, candidate.Scan().status);
    candidate = good;
    WriteU32(candidate.record.data() + kRecordingPositionBytes + kPackedEnvelopeBytes, 0);
    rehash(candidate);
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, candidate.Scan().status);
    candidate = good;
    candidate.record.back() = 1;
    rehash(candidate);
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, candidate.Scan().status);
    candidate = good;
    WriteU32(candidate.record.data() + kRecordingPositionBytes, 1000);
    WriteU32(candidate.record.data() + kRecordingPositionBytes + 8, VN_MONITOR_RECORD_FLAG_OPTIONAL);
    rehash(candidate);
    EXPECT_EQ(VN_OK, candidate.Scan().status);
}

TEST(MonitorTypedRecordingCodec, RejectsDuplicateDescriptorsEndsDataAfterEndAndRequiredChunkVersions) {
    const CodecCandidate candidate;
    const auto good = CodecFile(candidate.descriptor, candidate.end, {candidate.record});
    const auto scan = ScanSessionBytes(good.data(), good.size());
    ASSERT_EQ(3u, scan.commits.size());
    for (const auto index : {0u, 1u, 2u}) {
        auto file = good;
        std::vector<std::uint8_t> chunk;
        const auto& committed = scan.commits[index];
        PackSessionChunk(&chunk, committed.kind, VN_MONITOR_CHUNK_REQUIRED, 4,
            committed.payload.data(), committed.payload.size());
        file.insert(file.end(), chunk.begin(), chunk.end());
        EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, ScanSessionBytes(file.data(), file.size()).status);
    }
    auto prefix = CodecFile(candidate.descriptor, candidate.end, {}, true, false);
    std::vector<std::uint8_t> duplicate;
    PackSessionChunk(&duplicate, VN_MONITOR_CHUNK_EXECUTION_DESCRIPTOR, VN_MONITOR_CHUNK_REQUIRED, 2,
        scan.commits[0].payload.data(), scan.commits[0].payload.size());
    prefix.insert(prefix.end(), duplicate.begin(), duplicate.end());
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, ScanSessionBytes(prefix.data(), prefix.size()).status);
    std::size_t offset = kSessionFileHeaderPackedBytes;
    for (const auto& committed : scan.commits) {
        auto file = good;
        WriteU16(file.data() + offset + 4, 2);
        WriteU32(file.data() + offset + 28, Crc32C(file.data() + offset, 28));
        EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, ScanSessionBytes(file.data(), file.size()).status);
        offset += committed.rawBytes.size();
    }
}

TEST(MonitorTypedRecordingCodec, FiveMeasurementsMayShareOneSourceSequence) {
    CodecCandidate candidate;
    std::vector<std::vector<std::uint8_t>> records(5, candidate.record);
    candidate.end.checksum = kIdentityChecksumSeed;
    for (std::uint64_t i = 0; i != records.size(); ++i) {
        auto& record = records[i];
        WriteU64(record.data() + 8, i + 1);
        WriteU64(record.data() + kRecordingPositionBytes + 24, 101 + i);
        WriteU64(record.data() + kRecordingPositionBytes + 48, i + 1);
        candidate.end.checksum = IdentityChecksum(candidate.end.checksum, record.data(), static_cast<std::uint32_t>(record.size()));
    }
    candidate.end.ringEnds[1] = candidate.end.ringCompleted[1] = records.size();
    candidate.end.recordCount = records.size();
    const auto file = CodecFile(candidate.descriptor, candidate.end, records);
    const auto scan = ScanSessionBytes(file.data(), file.size());
    EXPECT_EQ(VN_OK, scan.status);
    EXPECT_EQ(0u, scan.recoveryNeeded);
}

TEST(MonitorTypedRecordingCodec, AcceptsProductionGainReductionAndRejectsItsMalformedDeclaredSize) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 913;
    tap.tapDescriptorGeneration = 7;
    tap.ownerStableId = harkentest::ObjectId(9);
    tap.ownerInstanceHandle = 8;
    tap.ownerGeneration = 7;
    VnMonitorMeasurementRequest request{};
    request.structSize = sizeof(request);
    request.kind = VN_MONITOR_MEASURE_GAIN_REDUCTION;
    request.channelMode = VN_MONITOR_CHANNEL_PER_CHANNEL;
    request.downsample = 1;
    struct LookupScope {
        LookupScope() {
            meter_test::SetGainReductionLookupForTests(
                [](const VnMonitorTapDescriptor& value, float* reduction, VnObjectId* stable,
                    std::uint64_t* instance, std::uint64_t* generation, void*) {
                    *reduction = 3.0f;
                    *stable = value.ownerStableId;
                    *instance = value.ownerInstanceHandle;
                    *generation = value.ownerGeneration;
                    return true;
                }, nullptr);
        }
        ~LookupScope() { meter_test::SetGainReductionLookupForTests(nullptr, nullptr); }
    } lookup;
    const std::array<float, 4> samples{0.1f, 0.2f, 0.3f, 0.4f};
    std::array<std::uint8_t, 1024> packed{};
    const auto bytes = PackAudioChunkRecord(packed.data(), packed.size(), 1, 256, 1,
        tap.tapDescriptorId, tap.tapDescriptorGeneration, 0, 48000, 1, 0, 0, 4, samples.data(), 4);
    ASSERT_NE(0u, bytes);
    std::vector<std::vector<std::uint8_t>> records;
    DispatchDerivedAudioChunk(&request, 1, &tap, 1, packed.data(), bytes,
        [](void* raw, const DerivedEmit& value) {
            auto& output = *static_cast<std::vector<std::vector<std::uint8_t>>*>(raw);
            output.emplace_back(value.bytes, value.bytes + value.size);
            return true;
        }, &records, 256);
    ASSERT_EQ(1u, records.size());
    VnMonitorRecordHeader header{};
    ASSERT_TRUE(ReadIdentityHeader(records[0].data(), records[0].size(), header));
    EXPECT_EQ(VN_MONITOR_RECORD_GAIN_REDUCTION, header.recordKind);
    EXPECT_EQ(48u, header.payloadBytes);
    CodecCandidate candidate;
    candidate.descriptor.streams = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    candidate.descriptor.permissions = VN_MONITOR_PERM_RECORD | VN_MONITOR_PERM_RAW_AUDIO;
    candidate.record.resize(kRecordingPositionBytes + records[0].size());
    PackRecordingPosition(candidate.record.data(), {RecordingSource::AudioRing, 0, 1, records[0].size()});
    std::copy(records[0].begin(), records[0].end(), candidate.record.begin() + kRecordingPositionBytes);
    WriteU64(candidate.record.data() + kRecordingPositionBytes + 48, 1);
    WriteU64(candidate.record.data() + kRecordingPositionBytes + 64, candidate.descriptor.epoch);
    candidate.end.checksum = IdentityChecksum(kIdentityChecksumSeed, candidate.record.data(),
        static_cast<std::uint32_t>(candidate.record.size()));
    EXPECT_EQ(VN_OK, candidate.Scan().status);
    EXPECT_EQ(0u, candidate.Scan().recoveryNeeded);
    WriteU32(candidate.record.data() + kRecordingPositionBytes + kPackedEnvelopeBytes, 48);
    candidate.end.checksum = IdentityChecksum(kIdentityChecksumSeed, candidate.record.data(),
        static_cast<std::uint32_t>(candidate.record.size()));
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, candidate.Scan().status);
}
