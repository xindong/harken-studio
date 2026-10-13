#include <harken/Harken.h>

#include "harken_remote/WireFormat.h"
#include "JsonReader.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern "C" VnResult vn_monitor_abi_pin_c_probe(void);

namespace {

constexpr uint32_t kPoison = 0xA5A5A5A5u;

VnConfig MakeNullConfig() {
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    return config;
}

} // namespace

static_assert(VN_MONITOR_WRONG_THREAD == -330, "monitor result must not renumber");
static_assert(VN_MONITOR_STALE_SESSION == -331, "monitor result must not renumber");
static_assert(VN_MONITOR_CONCURRENT_POLL == -332, "monitor result must not renumber");
static_assert(VN_MONITOR_BUFFER_TOO_SMALL == -333, "monitor result must not renumber");
static_assert(VN_MONITOR_UNSUPPORTED == -334, "monitor result must not renumber");
static_assert(VN_MONITOR_PERMISSION_DENIED == -335, "monitor result must not renumber");
static_assert(VN_MONITOR_STALE_LEASE == -336, "monitor result must not renumber");
static_assert(VN_MONITOR_STALE_PLAN == -337, "monitor result must not renumber");
static_assert(VN_MONITOR_STALE_TAP == -338, "monitor result must not renumber");
static_assert(VN_MONITOR_PLAN_CAPACITY == -339, "monitor result must not renumber");
static_assert(VN_MONITOR_SESSION_CAPACITY == -340, "monitor result must not renumber");
static_assert(VN_MONITOR_PRODUCER_CAPACITY == -341, "monitor result must not renumber");
static_assert(VN_MONITOR_STREAM_CAPACITY == -342, "monitor result must not renumber");
static_assert(VN_MONITOR_BUDGET_EXCEEDED == -343, "monitor result must not renumber");
static_assert(VN_MONITOR_INCOMPATIBLE == -344, "monitor result must not renumber");
static_assert(VN_MONITOR_SEQUENCE_EXHAUSTED == -345, "monitor result must not renumber");
static_assert(VN_MONITOR_RECORDING_FAILED == -346, "monitor result must not renumber");
static_assert(VN_MONITOR_RESYNC_REQUIRED == -347, "monitor result must not renumber");
static_assert(VN_MONITOR_STREAM_STATE == 1u << 0, "stream mask bit");
static_assert(VN_MONITOR_RECORD_UNKNOWN == 0, "record kind");
static_assert(VN_MONITOR_RECORD_MEASUREMENT_SEGMENT_RESET == 19, "segment reset kind");
static_assert(VN_MONITOR_RECORD_INVALID == 22, "invalid kind");
static_assert(VN_MONITOR_RECORD_EVENT_DETAIL == 23);
static_assert(sizeof(VnMonitorEventDetail) == 248);
static_assert(sizeof(VnMonitorEventDetailV2) == 264);
static_assert(offsetof(VnMonitorEventDetailV2, projectGuid) == 248);
static_assert(offsetof(VnMonitorEventDetail, timestampNs) == 24);
static_assert(offsetof(VnMonitorEventDetail, text) == 56);
static_assert(sizeof(VnMonitorEventSessionStatus) == 48);
static_assert(VN_MONITOR_DROP_SOURCE_PRODUCER == 1, "drop stage");
static_assert(VN_MONITOR_ENTITY_VOICE == 4, "entity kind");
static_assert(VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT == 1, "tap selector");
static_assert(VN_MONITOR_PERM_RAW_AUDIO == 1u << 5, "permission bit");
static_assert(VN_MONITOR_SECURITY_LAN_MTLS == 2, "security profile");
static_assert(VN_MONITOR_COMPARE_MISSING_DATA == 3, "comparison result");
static_assert(VN_MONITOR_PLAN_RETIRED == 2, "plan state");
static_assert(VN_MONITOR_STATE_CUT_ABORTED == 2, "state cut");
static_assert(VN_MONITOR_MEASURE_LUFS_INTEGRATED == 11, "measurement kind");
static_assert(VN_MONITOR_VALID_UNSUPPORTED == 1u << 3, "validity flag");
static_assert(VN_MONITOR_REGISTRY_CAPABILITY == 8, "registry kind");
static_assert(VN_MONITOR_REMOTE_OP_RECORDING == 6, "remote opcode");
static_assert(VN_MONITOR_RECORDING_STATE_FAILED == 3, "recording state");
static_assert(VN_MONITOR_CHANNEL_MONO_AVERAGE == 1, "channel mode");
static_assert(VN_MONITOR_CHUNK_OPTIONAL == 1u << 1, "chunk flag");
static_assert(VN_MONITOR_CHUNK_RECOVERY_INDEX == 13, "chunk kind");
static_assert(VN_MONITOR_SESSION_FORMAT_MAJOR == 1u, "file format major");
static_assert(VN_MONITOR_SESSION_FORMAT_MINOR == 0u, "file format minor");
static_assert(VN_MONITOR_SESSION_FILE_MAGIC == 0x30314e4f4d4e56ull, "VNMON10 LE");
static_assert(VN_MONITOR_SESSION_COMMIT_MAGIC == 0x3031544d434e56ull, "VNCMT10 LE");
static_assert(VN_MONITOR_SESSION_CHUNK_HEADER_BYTES == 32u, "on-disk packed chunk header");
static_assert(VN_MONITOR_SESSION_COMMIT_FOOTER_BYTES == 24u, "on-disk packed commit footer");
static_assert(sizeof(VnMonitorSessionChunkHeader) !=
                  VN_MONITOR_SESSION_CHUNK_HEADER_BYTES,
              "in-memory chunk header is not the packed file layout");
static_assert(sizeof(VnMonitorSessionCommitFooter) !=
                  VN_MONITOR_SESSION_COMMIT_FOOTER_BYTES,
              "in-memory commit footer is not the packed file layout");
static_assert(VN_MONITOR_SESSION_CHUNK_HEADER_BYTES == 32u, "PLAN-10 packed header");
static_assert(VN_MONITOR_SESSION_COMMIT_FOOTER_BYTES == 24u, "PLAN-10 packed footer");
static_assert(VN_MONITOR_DEFAULT_SESSION_FILE_BYTES == 8589934592ull, "8 GiB");
static_assert(VN_MONITOR_HARD_SESSION_FILE_BYTES == 68719476736ull, "64 GiB");
static_assert(VN_MONITOR_DEFAULT_CHUNK_PAYLOAD_BYTES == 67108864ull, "64 MiB");
static_assert(VN_MONITOR_HARD_CHUNK_PAYLOAD_BYTES == 268435456ull, "256 MiB");
static_assert(VN_MONITOR_HARD_REGISTRY_BLOB_BYTES == 65536u, "64 KiB");
static_assert(VN_MONITOR_REMOTE_MESSAGE_COUNTER_START == 1ull, "counter start");
static_assert(VN_MONITOR_REMOTE_NONCE_BYTES == 32u, "256-bit nonce");
static_assert(VN_MONITOR_REMOTE_SPKI_SHA256_BYTES == 32u, "SPKI hash");
static_assert(VN_MONITOR_DEFAULT_SESSIONS_PER_ENGINE == 8u, "capacity");
static_assert(VN_MONITOR_HARD_SESSIONS_PER_ENGINE == 32u, "capacity");
static_assert(VN_MONITOR_HARD_PROCESS_SESSION_SLOTS == 32u, "capacity");
static_assert(VN_MONITOR_DEFAULT_REMOTE_CLIENTS == 4u, "capacity");
static_assert(VN_MONITOR_HARD_REMOTE_CLIENTS == 16u, "capacity");
static_assert(VN_MONITOR_DEFAULT_PLAN_SLOTS == 8u, "capacity");
static_assert(VN_MONITOR_HARD_PLAN_SLOTS == 32u, "capacity");
static_assert(VN_MONITOR_DEDICATED_STOP_SLOTS == 1u, "capacity");
static_assert(VN_MONITOR_DEFAULT_PRODUCER_REGS == 128u, "capacity");
static_assert(VN_MONITOR_HARD_PRODUCER_REGS == 512u, "capacity");
static_assert(VN_MONITOR_DEFAULT_STREAMS == 512u, "capacity");
static_assert(VN_MONITOR_HARD_STREAMS == 2048u, "capacity");
static_assert(VN_MONITOR_DEFAULT_TAPS_PER_PLAN == 32u, "capacity");
static_assert(VN_MONITOR_HARD_TAPS_PER_PLAN == 128u, "capacity");
static_assert(VN_MONITOR_DEFAULT_MEASUREMENTS_PER_PLAN == 128u, "capacity");
static_assert(VN_MONITOR_HARD_MEASUREMENTS_PER_PLAN == 512u, "capacity");
static_assert(VN_MONITOR_DEFAULT_CONTROL_RECORD_BYTES == 4096u, "capacity");
static_assert(VN_MONITOR_HARD_CONTROL_RECORD_BYTES == 16384u, "capacity");
static_assert(VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES == 1024u, "capacity");
static_assert(VN_MONITOR_HARD_AUDIO_CHUNK_FRAMES == 4096u, "capacity");
static_assert(VN_MONITOR_DEFAULT_AUDIO_CHANNELS == 16u, "capacity");
static_assert(VN_MONITOR_HARD_AUDIO_CHANNELS == 64u, "capacity");
static_assert(VN_MONITOR_DEFAULT_PROCESS_QUEUE_BYTES == 536870912ull, "capacity");
static_assert(VN_MONITOR_HARD_PROCESS_QUEUE_BYTES == 536870912ull, "capacity");
static_assert(VN_MONITOR_DEFAULT_PRODUCER_RING_BYTES == 4194304ull, "capacity");
static_assert(VN_MONITOR_HARD_PRODUCER_RING_BYTES == 67108864ull, "capacity");
static_assert(VN_MONITOR_DEFAULT_CONSUMER_QUEUE_BYTES == 16777216ull, "capacity");
static_assert(VN_MONITOR_HARD_CONSUMER_QUEUE_BYTES == 134217728ull, "capacity");
static_assert(VN_MONITOR_DEFAULT_DERIVED_WORK_ITEMS == 1024u, "capacity");
static_assert(VN_MONITOR_HARD_DERIVED_WORK_ITEMS == 8192u, "capacity");
static_assert(VN_MONITOR_DEFAULT_RECORDER_QUEUE_BYTES == 67108864ull, "capacity");
static_assert(VN_MONITOR_HARD_RECORDER_QUEUE_BYTES == 536870912ull, "capacity");
static_assert(VN_MONITOR_DEFAULT_REMOTE_QUEUE_BYTES == 16777216ull, "capacity");
static_assert(VN_MONITOR_HARD_REMOTE_QUEUE_BYTES == 134217728ull, "capacity");
static_assert(VN_MONITOR_DEFAULT_CAPTURE_BYTE_RATE == 16777216ull, "capacity");
static_assert(VN_MONITOR_HARD_CAPTURE_BYTE_RATE == 134217728ull, "capacity");
static_assert(VN_MONITOR_DEFAULT_RAW_AUDIO_BYTE_RATE == 8388608ull, "capacity");
static_assert(VN_MONITOR_HARD_RAW_AUDIO_BYTE_RATE == 67108864ull, "capacity");
static_assert(offsetof(VnMonitorCapturePlan, taps) >
                  offsetof(VnMonitorCapturePlan, reserved1),
              "plan tap table appends");
static_assert(offsetof(VnMonitorSubscriptionStatus, baselines) >
                  offsetof(VnMonitorSubscriptionStatus, symbolsDecision),
              "subscription baselines append");


static_assert(offsetof(VnMonitorCapabilities, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorRecordHeader, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorSessionDesc, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorTelemetryCorrelation, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorDropSummary, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorStreamBaseline, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorMeasurementCapability, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorSubscriptionStatus, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorStateCutDesc, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorStateCutStatus, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorCaptureControlStatus, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorCapturePlan, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorEffectiveCapturePlan, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorPollDesc, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorPollResult, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorRecordingDesc, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorRecordingStatus, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorSessionCompareView, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorSessionCompareDesc, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorCompareResult, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorTapDescriptor, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorSessionFileHeader, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorSessionChunkHeader, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorSessionCommitFooter, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorRemoteHandshake, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorRemoteFrameCounter, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorStateCutBegin, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorStateUpsert, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorStateTombstone, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorStateCutEnd, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorEventPayload, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorTimeSeriesSample, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorAudioChunk, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorWaveformBucket, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorScopeResult, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorSpectrumPayload, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorMeterPayload, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorGainReductionPayload, structSize) == 0, "structSize first");
static_assert(offsetof(VnMonitorAggregatePayload, structSize) == 0, "structSize first");
static_assert(VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES == 316u, "packed envelope");
static_assert(sizeof(VnMonitorRecordHeader) !=
                  VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES,
              "in-memory header is not packed envelope");
static_assert(VN_MONITOR_RECORD_FLAG_REQUIRED == 1u << 0, "record flag");
static_assert(VN_MONITOR_RECORD_FLAG_OPTIONAL == 1u << 1, "record flag");
static_assert(VN_MONITOR_EVENT_DROP == 13, "event class");
static_assert(VN_MONITOR_CHECKSUM_CRC32C == 1, "checksum");
static_assert(alignof(VnMonitorRecordHeader) == 8, "record header align");
static_assert(alignof(VnMonitorCapabilities) == 8, "capabilities align");
static_assert(alignof(VnMonitorSessionFileHeader) == 8, "file header align");
static_assert(alignof(VnMonitorRemoteHandshake) == 8, "handshake align");
static_assert(sizeof(VnMonitorSessionHandle) == 8, "session handle");
static_assert(sizeof(uint32_t) == 4, "enums stay uint32");
static_assert(sizeof(VnConfig) == 352, "Monitor must not resize VnConfig");
static_assert(sizeof(VnMonitorRecordHeader) % 8 == 0, "record header size align");
static_assert(offsetof(VnMonitorRecordHeader, measurementSegmentGeneration) ==
                  offsetof(VnMonitorRecordHeader, clockAnchorGeneration) + 8,
              "segment generation appends after clock anchor");
static_assert(sizeof(VnMonitorRemoteHandshake{}.clientNonce) ==
                  VN_MONITOR_REMOTE_NONCE_BYTES,
              "nonce is 32 bytes");
static_assert(sizeof(VnMonitorRemoteHandshake{}.serverNonce) ==
                  VN_MONITOR_REMOTE_NONCE_BYTES,
              "server nonce is 32 bytes");

TEST(MonitorAbiPin, FileMagicIsEightByteAscii) {
    const char magic[8] = {"VNMON10"};
    uint64_t encoded = 0;
    std::memcpy(&encoded, magic, 8);
    EXPECT_EQ(VN_MONITOR_SESSION_FILE_MAGIC, encoded);
    const char commit[8] = {"VNCMT10"};
    std::memcpy(&encoded, commit, 8);
    EXPECT_EQ(VN_MONITOR_SESSION_COMMIT_MAGIC, encoded);
}

TEST(MonitorAbiPin, MonitorEntriesHaveNoRemoteOpcode) {
    const auto table = std::filesystem::path(HARKEN_TEMP_DIR).parent_path() /
        "HarkenEngine/Source/Tooling/Remote/wire/harken_wire_table.json";
    std::ifstream in(table);
    ASSERT_TRUE(in);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    harkenhost::JsonValue root;
    std::string error;
    ASSERT_TRUE(harkenhost::ParseJson(text, &root, &error)) << error;
    const auto* functions = root.Find("functions");
    ASSERT_NE(nullptr, functions);
    std::size_t opcodeCount = 0;
    for (const auto& function : functions->items) {
        const auto* opcode = function.Find("opcode");
        if (opcode == nullptr) continue;
        std::uint64_t value = 0;
        if (!opcode->AsU64(&value)) continue;
        ++opcodeCount;
        const auto* name = function.Find("name");
        ASSERT_NE(nullptr, name);
        EXPECT_NE(0u, name->text.find("Vn_Monitor")) << name->text;
    }
    EXPECT_EQ(harkenremote::kWireOpcodeCount, opcodeCount);
}

TEST(MonitorAbiPin, CTranslationUnitCanCallPublicAbi) {
    EXPECT_EQ(VN_NOT_INITIALIZED, vn_monitor_abi_pin_c_probe());
}

TEST(MonitorAbiPin, AllStubsReturnNotInitializedWithoutEngine) {
    VnMonitorCapabilities capabilities{};
    capabilities.structSize = sizeof(capabilities);
    VnMonitorSessionDesc sessionDesc{};
    sessionDesc.structSize = sizeof(sessionDesc);
    VnMonitorSessionHandle handle = 1;
    VnMonitorSubscriptionStatus subscription{};
    subscription.structSize = sizeof(subscription);
    uint32_t requiredBytes = kPoison;
    uint64_t revision = 1;
    uint8_t registry[8]{};
    uint64_t stateCutId = 1;
    VnMonitorStateCutDesc cutDesc{};
    cutDesc.structSize = sizeof(cutDesc);
    VnMonitorStateCutStatus cutStatus{};
    cutStatus.structSize = sizeof(cutStatus);
    VnMonitorCaptureControlStatus control{};
    control.structSize = sizeof(control);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    VnMonitorPollResult pollResult{};
    pollResult.structSize = sizeof(pollResult);
    VnMonitorDropSummary drop{};
    drop.structSize = sizeof(drop);
    uint32_t requiredCount = kPoison;
    VnMonitorRecordingDesc recordingDesc{};
    recordingDesc.structSize = sizeof(recordingDesc);
    VnMonitorRecordingStatus recordingStatus{};
    recordingStatus.structSize = sizeof(recordingStatus);

    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorGetCapabilities(&capabilities));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorOpenSession(&sessionDesc, &handle));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorCloseSession(handle));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorGetSubscriptionStatus(handle, &subscription));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorCaptureRegistry(VN_MONITOR_REGISTRY_REASON, registry, sizeof(registry), &requiredBytes, &revision));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorRequestStateCut(handle, &cutDesc, &stateCutId));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorGetStateCutStatus(handle, stateCutId, &cutStatus));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorAcquireCaptureControl(handle, &control));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorReleaseCaptureControl(handle, 1));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorGetCaptureControlStatus(handle, &control));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorValidateCapturePlan(handle, 1, &plan, &accepted));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorApplyCapturePlan(handle, 1, &plan, &accepted));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorStopCapture(handle, 1, &accepted));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorGetEffectiveCapturePlan(handle, 1, &accepted));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorPoll(handle, &pollDesc, &pollResult));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorGetConsumerStatus(handle, &drop, 1, &requiredCount));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorStartRecording(handle, 1, &recordingDesc));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorStopRecording(handle, 1));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorGetRecordingStatus(handle, &recordingStatus));
    EXPECT_EQ(kPoison, requiredBytes);
    EXPECT_EQ(kPoison, requiredCount);
}

TEST(MonitorAbiPin, UnimplementedEntriesReturnUnsupportedWhenEngineExistsWithoutWriting) {
    const VnConfig config = MakeNullConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&config));

    VnMonitorCapabilities capabilities{};
    capabilities.structSize = sizeof(capabilities);
    capabilities.monitorImplemented = kPoison;
    VnMonitorSessionDesc sessionDesc{};
    sessionDesc.structSize = sizeof(sessionDesc);
    VnMonitorSessionHandle handle = 0;
    VnMonitorSubscriptionStatus subscription{};
    subscription.structSize = sizeof(subscription);
    uint32_t requiredBytes = kPoison;
    uint64_t revision = 7;
    uint8_t registry[8];
    std::memset(registry, 0x5A, sizeof(registry));
    uint64_t stateCutId = 9;
    VnMonitorStateCutDesc cutDesc{};
    cutDesc.structSize = sizeof(cutDesc);
    VnMonitorStateCutStatus cutStatus{};
    cutStatus.structSize = sizeof(cutStatus);
    VnMonitorCaptureControlStatus control{};
    control.structSize = sizeof(control);
    control.held = kPoison;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    accepted.activationState = kPoison;
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    VnMonitorPollResult pollResult{};
    pollResult.structSize = sizeof(pollResult);
    pollResult.recordsWritten = kPoison;
    VnMonitorDropSummary drop{};
    drop.structSize = sizeof(drop);
    drop.recordCount = 11;
    uint32_t requiredCount = kPoison;
    VnMonitorRecordingDesc recordingDesc{};
    recordingDesc.structSize = sizeof(recordingDesc);
    VnMonitorRecordingStatus recordingStatus{};
    recordingStatus.structSize = sizeof(recordingStatus);
    recordingStatus.state = kPoison;

    EXPECT_EQ(VN_MONITOR_UNSUPPORTED, Vn_MonitorCaptureRegistry(VN_MONITOR_REGISTRY_REASON, registry, sizeof(registry), &requiredBytes, &revision));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorRequestStateCut(1, &cutDesc, &stateCutId));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorGetStateCutStatus(1, stateCutId, &cutStatus));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorAcquireCaptureControl(1, &control));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorReleaseCaptureControl(1, 1));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorGetCaptureControlStatus(1, &control));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorValidateCapturePlan(1, 1, &plan, &accepted));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorApplyCapturePlan(1, 1, &plan, &accepted));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorStopCapture(1, 1, &accepted));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorGetEffectiveCapturePlan(1, 1, &accepted));
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorStartRecording(1, 1, &recordingDesc));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorStopRecording(1, 1));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorGetRecordingStatus(1, &recordingStatus));

    EXPECT_EQ(kPoison, requiredBytes);
    EXPECT_EQ(7u, revision);
    EXPECT_EQ(0x5A, registry[0]);
    EXPECT_EQ(9u, stateCutId);
    EXPECT_EQ(kPoison, control.held);
    EXPECT_EQ(kPoison, accepted.activationState);
    EXPECT_EQ(kPoison, pollResult.recordsWritten);
    EXPECT_EQ(11u, drop.recordCount);
    EXPECT_EQ(kPoison, requiredCount);
    EXPECT_EQ(kPoison, recordingStatus.state);
    EXPECT_EQ(VN_OK, Vn_Term());
}

// Stable M0-6 appendix pin. Later packages must keep this TEST() name: it is
// the RuntimeVerified id for "Open G-gaps stay Unsupported". Do not assert
// GetCapabilities failure (M1 makes that a live path). Do not reuse
// AllStubsReturnNotInitializedWithoutEngine or any M1-renamed helper.
TEST(MonitorAbiPin, OpenGGapsStayUnsupportedWhenEngineExistsWithoutWriting) {
    const VnConfig config = MakeNullConfig();
    ASSERT_EQ(VN_OK, Vn_Init(&config));

    uint32_t requiredBytes = kPoison;
    uint64_t revision = 7;
    uint8_t registry[8];
    std::memset(registry, 0x5A, sizeof(registry));
    uint64_t stateCutId = 9;
    VnMonitorStateCutDesc cutDesc{};
    cutDesc.structSize = sizeof(cutDesc);
    VnMonitorStateCutStatus cutStatus{};
    cutStatus.structSize = sizeof(cutStatus);
    VnMonitorCaptureControlStatus control{};
    control.structSize = sizeof(control);
    control.held = kPoison;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    accepted.activationState = kPoison;
    VnMonitorRecordingDesc recordingDesc{};
    recordingDesc.structSize = sizeof(recordingDesc);
    VnMonitorRecordingStatus recordingStatus{};
    recordingStatus.structSize = sizeof(recordingStatus);
    recordingStatus.state = kPoison;

    EXPECT_EQ(VN_MONITOR_UNSUPPORTED, Vn_MonitorCaptureRegistry(VN_MONITOR_REGISTRY_REASON, registry, sizeof(registry), &requiredBytes, &revision));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorRequestStateCut(1, &cutDesc, &stateCutId));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorGetStateCutStatus(1, stateCutId, &cutStatus));
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorStartRecording(1, 1, &recordingDesc));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorStopRecording(1, 1));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorGetRecordingStatus(1, &recordingStatus));

    EXPECT_EQ(kPoison, requiredBytes);
    EXPECT_EQ(7u, revision);
    EXPECT_EQ(0x5A, registry[0]);
    EXPECT_EQ(9u, stateCutId);
    EXPECT_EQ(kPoison, control.held);
    EXPECT_EQ(kPoison, accepted.activationState);
    EXPECT_EQ(kPoison, recordingStatus.state);
    EXPECT_EQ(VN_OK, Vn_Term());
}
