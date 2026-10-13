#include "MonitorSessionFileWrite.h"
#include "MonitorSessionRecover.h"
#include "MonitorCaptureHubTestHooks.h"
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
    const auto dir = std::filesystem::temp_directory_path() / "harken-monitor-m42";
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

void WriteAll(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out);
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
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

std::string RecordCompleteFile(const char* name) {
    const std::string path = TempSessionPath(name);
    RemoveIfExists(path);
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    EXPECT_EQ(VN_OK, engine.InitResult());
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
    return path;
}

std::uint64_t Fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::uint8_t b : bytes) {
        hash ^= b;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

TEST(MonitorSession, RecoverCompleteM41FileKeepsAllCommits) {
    const std::string path = RecordCompleteFile("complete.vnmon");
    const auto before = ReadAll(path);
    const auto hash = Fnv1a64(before);
    const auto recovered = harken::monitor::RecoverSessionFile(path.c_str());
    EXPECT_EQ(VN_OK, recovered.status);
    EXPECT_GE(recovered.commitsWritten, 1u);
    EXPECT_EQ(recovered.bytesConsumed, before.size());
    EXPECT_EQ(0u, recovered.recoveryNeeded);
    EXPECT_EQ(hash, Fnv1a64(ReadAll(path)));
    RemoveIfExists(path);
}

TEST(MonitorSession, RecoverTruncatedPayloadKeepsLastCompleteCommit) {
    const std::string path = RecordCompleteFile("cut-payload.vnmon");
    auto bytes = ReadAll(path);
    ASSERT_GT(bytes.size(), 64u + 32u);
    bytes.resize(bytes.size() - 8);
    WriteAll(path, bytes);
    const auto hash = Fnv1a64(bytes);
    const auto recovered = harken::monitor::RecoverSessionFile(path.c_str());
    EXPECT_EQ(VN_OK, recovered.status);
    EXPECT_EQ(1u, recovered.recoveryNeeded);
    EXPECT_LT(recovered.bytesConsumed, bytes.size());
    EXPECT_GE(recovered.commitsWritten, 1u);
    EXPECT_EQ(hash, Fnv1a64(ReadAll(path)));
    RemoveIfExists(path);
}

TEST(MonitorSession, RecoverHalfFooterDoesNotCommitTail) {
    const std::string path = RecordCompleteFile("cut-footer.vnmon");
    auto bytes = ReadAll(path);
    ASSERT_GT(bytes.size(), 64u + 32u + 24u);
    bytes.resize(bytes.size() - 12);
    WriteAll(path, bytes);
    const auto recovered = harken::monitor::RecoverSessionFile(path.c_str());
    EXPECT_EQ(VN_OK, recovered.status);
    EXPECT_EQ(1u, recovered.recoveryNeeded);
    EXPECT_LT(recovered.bytesConsumed, bytes.size());
    RemoveIfExists(path);
}

TEST(MonitorSession, RecoverMissingFooterKeepsPriorCommits) {
    std::vector<std::uint8_t> bytes(64 + 40, 0);
    harken::monitor::PackSessionFileHeader(bytes.data(), 1, 1, 0, 0);
    std::vector<std::uint8_t> chunk;
    const std::uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    harken::monitor::PackSessionChunk(&chunk, VN_MONITOR_CHUNK_EVENTS,
                                     VN_MONITOR_CHUNK_REQUIRED, 1, payload, 8);
    bytes.insert(bytes.begin() + 64, chunk.begin(), chunk.begin() + 40);
    const auto recovered =
        harken::monitor::ScanSessionBytes(bytes.data(), bytes.size());
    EXPECT_EQ(VN_OK, recovered.status);
    EXPECT_EQ(0u, recovered.commitsWritten);
    EXPECT_EQ(1u, recovered.recoveryNeeded);
    EXPECT_EQ(64u, recovered.bytesConsumed);
}

TEST(MonitorSession, RecoverCrcFailedRequiredDropsCommitAndContinues) {
    std::vector<std::uint8_t> header(64, 0);
    harken::monitor::PackSessionFileHeader(header.data(), 1, 1, 0, 0);
    std::vector<std::uint8_t> first;
    const std::uint8_t payloadA[4] = {9, 9, 9, 9};
    harken::monitor::PackSessionChunk(&first, VN_MONITOR_CHUNK_EVENTS,
                                     VN_MONITOR_CHUNK_REQUIRED, 1, payloadA, 4);
    first[32] ^= 0xff;
    std::vector<std::uint8_t> second;
    const std::uint8_t payloadB[4] = {1, 2, 3, 4};
    harken::monitor::PackSessionChunk(&second, VN_MONITOR_CHUNK_EVENTS,
                                     VN_MONITOR_CHUNK_REQUIRED, 2, payloadB, 4);
    std::vector<std::uint8_t> bytes = header;
    bytes.insert(bytes.end(), first.begin(), first.end());
    bytes.insert(bytes.end(), second.begin(), second.end());
    const auto recovered =
        harken::monitor::ScanSessionBytes(bytes.data(), bytes.size());
    EXPECT_EQ(VN_OK, recovered.status);
    EXPECT_EQ(1u, recovered.droppedRequired);
    ASSERT_EQ(1u, recovered.commitsWritten);
    EXPECT_EQ(2u, recovered.commits[0].chunkSequence);
}

TEST(MonitorSession, RecoverRequiredUnknownIsIncompatible) {
    std::vector<std::uint8_t> header(64, 0);
    harken::monitor::PackSessionFileHeader(header.data(), 1, 1, 0, 0);
    std::vector<std::uint8_t> chunk;
    const std::uint8_t payload[4] = {1, 2, 3, 4};
    harken::monitor::PackSessionChunk(&chunk, 200, VN_MONITOR_CHUNK_REQUIRED, 1, payload,
                                     4);
    std::vector<std::uint8_t> bytes = header;
    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    const auto recovered =
        harken::monitor::ScanSessionBytes(bytes.data(), bytes.size());
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, recovered.status);
}

TEST(MonitorSession, RecoverOptionalUnknownKeptAndCopiedByteExact) {
    std::vector<std::uint8_t> header(64, 0);
    harken::monitor::PackSessionFileHeader(header.data(), 1, 1, 0, 0);
    std::vector<std::uint8_t> unknown;
    const std::uint8_t payload[4] = {0x11, 0x22, 0x33, 0x44};
    harken::monitor::PackSessionChunk(&unknown, 200, VN_MONITOR_CHUNK_OPTIONAL, 1,
                                     payload, 4);
    std::vector<std::uint8_t> events;
    const std::uint8_t eventPayload[4] = {5, 6, 7, 8};
    harken::monitor::PackSessionChunk(&events, VN_MONITOR_CHUNK_EVENTS,
                                     VN_MONITOR_CHUNK_REQUIRED, 2, eventPayload, 4);
    std::vector<std::uint8_t> bytes = header;
    bytes.insert(bytes.end(), unknown.begin(), unknown.end());
    bytes.insert(bytes.end(), events.begin(), events.end());
    const std::string source = TempSessionPath("optional-unknown.vnmon");
    const std::string dest = TempSessionPath("optional-unknown.copy.vnmon");
    RemoveIfExists(source);
    RemoveIfExists(dest);
    WriteAll(source, bytes);
    const auto recovered = harken::monitor::RecoverSessionFile(source.c_str());
    EXPECT_EQ(VN_OK, recovered.status);
    ASSERT_EQ(2u, recovered.commitsWritten);
    EXPECT_EQ(200u, recovered.commits[0].kind);
    ASSERT_EQ(4u, recovered.commits[0].payload.size());
    EXPECT_EQ(0x11, recovered.commits[0].payload[0]);
    EXPECT_EQ(VN_OK, harken::monitor::CopyRecoveredSessionFile(source.c_str(),
                                                              dest.c_str()));
    EXPECT_EQ(ReadAll(source), ReadAll(dest));
    RemoveIfExists(source);
    RemoveIfExists(dest);
}

TEST(MonitorSession, RecoverNativeStructMirrorIsIncompatible) {
    VnMonitorSessionFileHeader native{};
    native.structSize = sizeof(native);
    native.fileMagic = VN_MONITOR_SESSION_FILE_MAGIC;
    std::vector<std::uint8_t> bytes(sizeof(native), 0);
    std::memcpy(bytes.data(), &native, sizeof(native));
    const auto recovered =
        harken::monitor::ScanSessionBytes(bytes.data(), bytes.size());
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, recovered.status);
}

TEST(MonitorSession, RecoverRealTraceFileIsIncompatible) {
    std::vector<std::uint8_t> bytes(64, 0);
    bytes[0] = 'V';
    bytes[1] = 'T';
    bytes[2] = 'R';
    bytes[3] = 'C';
    bytes[4] = 5;
    const auto recovered =
        harken::monitor::ScanSessionBytes(bytes.data(), bytes.size());
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, recovered.status);
}

TEST(MonitorSession, RecoverHeaderOnlyNeedsRecoveryAndZeroCommits) {
    std::vector<std::uint8_t> bytes(64, 0);
    harken::monitor::PackSessionFileHeader(bytes.data(), 1, 1, 0, 0);
    const auto recovered =
        harken::monitor::ScanSessionBytes(bytes.data(), bytes.size());
    EXPECT_EQ(VN_OK, recovered.status);
    EXPECT_EQ(0u, recovered.commitsWritten);
    EXPECT_EQ(1u, recovered.recoveryNeeded);
    EXPECT_EQ(64u, recovered.bytesConsumed);
}

TEST(MonitorSession, RecoverEmptyFileIsIncompatible) {
    const auto recovered = harken::monitor::ScanSessionBytes(nullptr, 0);
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, recovered.status);
}

TEST(MonitorSession, RecoverCopyKeepsCrcFailedRequiredChunkInPrefix) {
    std::vector<std::uint8_t> header(64, 0);
    harken::monitor::PackSessionFileHeader(header.data(), 1, 1, 0, 0);
    std::vector<std::uint8_t> first;
    const std::uint8_t payloadA[4] = {9, 9, 9, 9};
    harken::monitor::PackSessionChunk(&first, VN_MONITOR_CHUNK_EVENTS,
                                     VN_MONITOR_CHUNK_REQUIRED, 1, payloadA, 4);
    first[32] ^= 0xff;
    std::vector<std::uint8_t> second;
    const std::uint8_t payloadB[4] = {1, 2, 3, 4};
    harken::monitor::PackSessionChunk(&second, VN_MONITOR_CHUNK_EVENTS,
                                     VN_MONITOR_CHUNK_REQUIRED, 2, payloadB, 4);
    std::vector<std::uint8_t> bytes = header;
    bytes.insert(bytes.end(), first.begin(), first.end());
    bytes.insert(bytes.end(), second.begin(), second.end());
    const std::string source = TempSessionPath("crc-prefix.vnmon");
    const std::string dest = TempSessionPath("crc-prefix.copy.vnmon");
    RemoveIfExists(source);
    RemoveIfExists(dest);
    WriteAll(source, bytes);
    EXPECT_EQ(VN_OK, harken::monitor::CopyRecoveredSessionFile(source.c_str(),
                                                              dest.c_str()));
    EXPECT_EQ(bytes, ReadAll(dest));
    const auto recovered = harken::monitor::RecoverSessionFile(dest.c_str());
    EXPECT_EQ(1u, recovered.droppedRequired);
    ASSERT_EQ(1u, recovered.commitsWritten);
    EXPECT_EQ(2u, recovered.commits[0].chunkSequence);
    RemoveIfExists(source);
    RemoveIfExists(dest);
}

TEST(MonitorSession, RecoverMatchesHarnessScanOnTruncatedM41File) {
    const std::string path = RecordCompleteFile("dual-scan.vnmon");
    auto bytes = ReadAll(path);
    ASSERT_GT(bytes.size(), 64u + 32u);
    bytes.resize(bytes.size() - 8);
    WriteAll(path, bytes);
    const auto recovered = harken::monitor::ScanSessionBytes(bytes.data(), bytes.size());
    VnMonitorSessionCommitView views[16]{};
    VnMonitorDecodedRecord scratch[16]{};
    std::uint8_t skip[4096]{};
    const auto harness = vn_monitor_scan_session(bytes.data(), bytes.size(), views, 16,
                                                 scratch, 16, skip, sizeof(skip));
    EXPECT_EQ(harness.status, recovered.status);
    EXPECT_EQ(harness.commitsWritten, recovered.commitsWritten);
    EXPECT_EQ(harness.bytesConsumed, recovered.bytesConsumed);
    EXPECT_EQ(harness.droppedRequired, recovered.droppedRequired);
    RemoveIfExists(path);
}

TEST(MonitorSession, RecoverCopyLeavesSourceUnchangedAndCleansFailedPart) {
    const std::string source = RecordCompleteFile("copy-src.vnmon");
    const std::string dest = TempSessionPath("copy-dst.vnmon");
    RemoveIfExists(dest);
    const auto before = ReadAll(source);
    EXPECT_EQ(VN_OK, harken::monitor::CopyRecoveredSessionFile(source.c_str(),
                                                              dest.c_str()));
    EXPECT_EQ(before, ReadAll(source));
    EXPECT_EQ(VN_IN_USE, harken::monitor::CopyRecoveredSessionFile(source.c_str(),
                                                                  dest.c_str()));
    EXPECT_EQ(before, ReadAll(dest));
    RemoveIfExists(source);
    RemoveIfExists(dest);
}

TEST(MonitorSession, RecoverCopyRenameRaceKeepsExistingDestination) {
    const std::string source = RecordCompleteFile("copy-race-src.vnmon");
    const std::string dest = TempSessionPath("copy-race-dst.vnmon");
    const std::string part = dest + ".part";
    RemoveIfExists(dest);
    RemoveIfExists(part);
    const std::vector<std::uint8_t> marker = {'R', 'A', 'C', 'E'};
    WriteAll(dest, marker);
    EXPECT_EQ(VN_IN_USE, harken::monitor::CopyRecoveredSessionFile(source.c_str(),
                                                                  dest.c_str()));
    EXPECT_EQ(marker, ReadAll(dest));
    EXPECT_FALSE(std::filesystem::exists(part));
    RemoveIfExists(source);
    RemoveIfExists(dest);
    RemoveIfExists(part);
}
