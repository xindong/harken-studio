#include "monitor_fixture_decode.h"

#include "JsonReader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#ifndef HARKEN_MONITOR_PLAN_FIXTURE_DIR
#define HARKEN_MONITOR_PLAN_FIXTURE_DIR "."
#endif

namespace {

std::string FixturePath(const char* name) {
    return std::string(HARKEN_MONITOR_PLAN_FIXTURE_DIR) + "/" + name;
}

std::vector<uint8_t> ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << path;
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

std::string ReadText(const std::string& path) {
    std::ifstream in(path);
    EXPECT_TRUE(in) << path;
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

harkenhost::JsonValue ParseFile(const char* name) {
    harkenhost::JsonValue root;
    std::string error;
    const std::string text = ReadText(FixturePath(name));
    EXPECT_TRUE(harkenhost::ParseJson(text, &root, &error)) << error;
    return root;
}

uint64_t RequireU64(const harkenhost::JsonValue& object, const char* key) {
    const harkenhost::JsonValue* value = object.Find(key);
    EXPECT_NE(value, nullptr) << key;
    uint64_t out = 0;
    EXPECT_TRUE(value != nullptr && value->AsU64(&out)) << key;
    return out;
}

std::string RequireString(const harkenhost::JsonValue& object, const char* key) {
    const harkenhost::JsonValue* value = object.Find(key);
    EXPECT_NE(value, nullptr) << key;
    EXPECT_EQ(value != nullptr ? value->kind : harkenhost::JsonValue::Kind::Null,
              harkenhost::JsonValue::Kind::String)
        << key;
    return value ? value->text : std::string();
}

const harkenhost::JsonValue* RequireArray(const harkenhost::JsonValue& object,
                                         const char* key) {
    const harkenhost::JsonValue* value = object.Find(key);
    EXPECT_NE(value, nullptr) << key;
    EXPECT_EQ(value != nullptr ? value->kind : harkenhost::JsonValue::Kind::Null,
              harkenhost::JsonValue::Kind::Array)
        << key;
    return value;
}

void ExpectSameU64(const harkenhost::JsonValue& before,
                   const harkenhost::JsonValue& after,
                   const char* key) {
    EXPECT_EQ(RequireU64(after, key), RequireU64(before, key)) << key;
}

void ExpectU64Array(const harkenhost::JsonValue& object,
                    const char* key,
                    std::initializer_list<uint64_t> wanted) {
    const harkenhost::JsonValue* values = RequireArray(object, key);
    ASSERT_NE(values, nullptr) << key;
    ASSERT_EQ(values->items.size(), wanted.size()) << key;
    size_t i = 0;
    for (uint64_t expected : wanted) {
        uint64_t got = 0;
        EXPECT_TRUE(values->items[i].AsU64(&got)) << key;
        EXPECT_EQ(got, expected) << key;
        ++i;
    }
}

const harkenhost::JsonValue* FindSequence(const harkenhost::JsonValue& root,
                                         const char* id) {
    const harkenhost::JsonValue* sequences = RequireArray(root, "sequences");
    if (sequences == nullptr)
        return nullptr;
    for (const auto& item : sequences->items) {
        const harkenhost::JsonValue* found = item.Find("id");
        if (found != nullptr && found->text == id)
            return &item;
    }
    ADD_FAILURE() << "missing sequence " << id;
    return nullptr;
}

void DecodeCheckedIn(const char* name,
                     uint32_t capacity,
                     std::vector<uint8_t>* bytes,
                     std::vector<VnMonitorDecodedRecord>* out) {
    *bytes = ReadAll(FixturePath(name));
    ASSERT_GE(bytes->size(), 8u);
    uint64_t count = 0;
    std::memcpy(&count, bytes->data(), 8);
    ASSERT_LE(count, static_cast<uint64_t>(capacity));
    out->assign(capacity, VnMonitorDecodedRecord{});
    uint8_t skip[8]{};
    auto result = vn_monitor_decode_records(bytes->data() + 8, bytes->size() - 8,
                                            out->data(), capacity, skip,
                                            sizeof(skip));
    ASSERT_EQ(result.status, VN_OK);
    ASSERT_EQ(result.recordsWritten, count);
    out->resize(static_cast<size_t>(result.recordsWritten));
    for (const auto& rec : *out)
        EXPECT_EQ(rec.header.deliverySequence, 0u);
}

} // namespace

TEST(MonitorFixture, PlanCapacityConstantsMatchContract) {
    const auto expected = ParseFile("capacity.expected.json");
    const harkenhost::JsonValue* defaults = expected.Find("contractDefault");
    const harkenhost::JsonValue* hard = expected.Find("hard");
    ASSERT_NE(defaults, nullptr);
    ASSERT_NE(hard, nullptr);
    EXPECT_EQ(RequireU64(*defaults, "sessionsPerEngine"),
              VN_MONITOR_DEFAULT_SESSIONS_PER_ENGINE);
    EXPECT_EQ(RequireU64(*hard, "sessionsPerEngine"),
              VN_MONITOR_HARD_SESSIONS_PER_ENGINE);
    EXPECT_EQ(RequireU64(*hard, "processSessionSlots"),
              VN_MONITOR_HARD_PROCESS_SESSION_SLOTS);
    EXPECT_EQ(RequireU64(*defaults, "processQueueBytes"),
              VN_MONITOR_DEFAULT_PROCESS_QUEUE_BYTES);
    EXPECT_EQ(RequireU64(*hard, "processQueueBytes"),
              VN_MONITOR_HARD_PROCESS_QUEUE_BYTES);
    EXPECT_EQ(RequireU64(*defaults, "remoteClients"),
              VN_MONITOR_DEFAULT_REMOTE_CLIENTS);
    EXPECT_EQ(RequireU64(*hard, "remoteClients"), VN_MONITOR_HARD_REMOTE_CLIENTS);
    EXPECT_EQ(RequireU64(*defaults, "planSlots"), VN_MONITOR_DEFAULT_PLAN_SLOTS);
    EXPECT_EQ(RequireU64(*hard, "planSlots"), VN_MONITOR_HARD_PLAN_SLOTS);
    EXPECT_EQ(RequireU64(*defaults, "dedicatedStopSlots"),
              VN_MONITOR_DEDICATED_STOP_SLOTS);
    EXPECT_EQ(RequireU64(*hard, "dedicatedStopSlots"),
              VN_MONITOR_DEDICATED_STOP_SLOTS);
    EXPECT_EQ(RequireU64(*defaults, "producerRegs"),
              VN_MONITOR_DEFAULT_PRODUCER_REGS);
    EXPECT_EQ(RequireU64(*hard, "producerRegs"), VN_MONITOR_HARD_PRODUCER_REGS);
    EXPECT_EQ(RequireU64(*defaults, "streams"), VN_MONITOR_DEFAULT_STREAMS);
    EXPECT_EQ(RequireU64(*hard, "streams"), VN_MONITOR_HARD_STREAMS);
    EXPECT_EQ(RequireU64(*defaults, "tapsPerPlan"),
              VN_MONITOR_DEFAULT_TAPS_PER_PLAN);
    EXPECT_EQ(RequireU64(*hard, "tapsPerPlan"), VN_MONITOR_HARD_TAPS_PER_PLAN);
    EXPECT_EQ(RequireU64(*defaults, "measurementsPerPlan"),
              VN_MONITOR_DEFAULT_MEASUREMENTS_PER_PLAN);
    EXPECT_EQ(RequireU64(*hard, "measurementsPerPlan"),
              VN_MONITOR_HARD_MEASUREMENTS_PER_PLAN);
    EXPECT_EQ(RequireU64(*defaults, "controlRecordBytes"),
              VN_MONITOR_DEFAULT_CONTROL_RECORD_BYTES);
    EXPECT_EQ(RequireU64(*hard, "controlRecordBytes"),
              VN_MONITOR_HARD_CONTROL_RECORD_BYTES);
    EXPECT_EQ(RequireU64(*defaults, "audioChunkFrames"),
              VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES);
    EXPECT_EQ(RequireU64(*hard, "audioChunkFrames"),
              VN_MONITOR_HARD_AUDIO_CHUNK_FRAMES);
    EXPECT_EQ(RequireU64(*defaults, "audioChannels"),
              VN_MONITOR_DEFAULT_AUDIO_CHANNELS);
    EXPECT_EQ(RequireU64(*hard, "audioChannels"), VN_MONITOR_HARD_AUDIO_CHANNELS);
    EXPECT_EQ(RequireU64(*defaults, "producerRingBytes"),
              VN_MONITOR_DEFAULT_PRODUCER_RING_BYTES);
    EXPECT_EQ(RequireU64(*hard, "producerRingBytes"),
              VN_MONITOR_HARD_PRODUCER_RING_BYTES);
    EXPECT_EQ(RequireU64(*defaults, "consumerQueueBytes"),
              VN_MONITOR_DEFAULT_CONSUMER_QUEUE_BYTES);
    EXPECT_EQ(RequireU64(*hard, "consumerQueueBytes"),
              VN_MONITOR_HARD_CONSUMER_QUEUE_BYTES);
    EXPECT_EQ(RequireU64(*defaults, "derivedWorkItems"),
              VN_MONITOR_DEFAULT_DERIVED_WORK_ITEMS);
    EXPECT_EQ(RequireU64(*hard, "derivedWorkItems"),
              VN_MONITOR_HARD_DERIVED_WORK_ITEMS);
    EXPECT_EQ(RequireU64(*defaults, "recorderQueueBytes"),
              VN_MONITOR_DEFAULT_RECORDER_QUEUE_BYTES);
    EXPECT_EQ(RequireU64(*hard, "recorderQueueBytes"),
              VN_MONITOR_HARD_RECORDER_QUEUE_BYTES);
    EXPECT_EQ(RequireU64(*defaults, "remoteQueueBytes"),
              VN_MONITOR_DEFAULT_REMOTE_QUEUE_BYTES);
    EXPECT_EQ(RequireU64(*hard, "remoteQueueBytes"),
              VN_MONITOR_HARD_REMOTE_QUEUE_BYTES);
    EXPECT_EQ(RequireU64(*defaults, "captureByteRate"),
              VN_MONITOR_DEFAULT_CAPTURE_BYTE_RATE);
    EXPECT_EQ(RequireU64(*hard, "captureByteRate"),
              VN_MONITOR_HARD_CAPTURE_BYTE_RATE);
    EXPECT_EQ(RequireU64(*defaults, "rawAudioByteRate"),
              VN_MONITOR_DEFAULT_RAW_AUDIO_BYTE_RATE);
    EXPECT_EQ(RequireU64(*hard, "rawAudioByteRate"),
              VN_MONITOR_HARD_RAW_AUDIO_BYTE_RATE);
    EXPECT_EQ(RequireU64(*defaults, "sessionFileBytes"),
              VN_MONITOR_DEFAULT_SESSION_FILE_BYTES);
    EXPECT_EQ(RequireU64(*hard, "sessionFileBytes"),
              VN_MONITOR_HARD_SESSION_FILE_BYTES);

    const harkenhost::JsonValue* resolved = expected.Find("resolvedSample");
    ASSERT_NE(resolved, nullptr);
    EXPECT_LT(RequireU64(*resolved, "sessionsPerEngine"),
              VN_MONITOR_HARD_SESSIONS_PER_ENGINE);
    EXPECT_NE(RequireU64(*resolved, "sessionsPerEngine"),
              VN_MONITOR_DEFAULT_SESSIONS_PER_ENGINE);

    const harkenhost::JsonValue* errors = expected.Find("errorContract");
    ASSERT_NE(errors, nullptr);
    EXPECT_EQ(RequireString(*errors, "sessionCapacity"),
              "VN_MONITOR_SESSION_CAPACITY");
    EXPECT_EQ(VN_MONITOR_SESSION_CAPACITY, -340);
    EXPECT_EQ(VN_MONITOR_PLAN_CAPACITY, -339);
    EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED, -343);
}

TEST(MonitorFixture, PlanRegistryRevisionKeepsOldDescriptor) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorDecodedRecord> recs;
    DecodeCheckedIn("registry.bin", 4, &bytes, &recs);
    ASSERT_EQ(recs.size(), 4u);
    EXPECT_EQ(recs[0].header.recordKind, VN_MONITOR_RECORD_REGISTRY_REVISION);
    EXPECT_EQ(recs[1].header.recordKind, VN_MONITOR_RECORD_REGISTRY_REVISION);
    EXPECT_EQ(recs[0].header.stableId, recs[1].header.stableId);
    EXPECT_NE(recs[0].header.instanceGeneration, recs[1].header.instanceGeneration);
    EXPECT_NE(recs[0].header.instanceHandle, recs[1].header.instanceHandle);
    EXPECT_EQ(recs[2].header.recordKind, VN_MONITOR_RECORD_TAP_BINDING_CHANGED);
    EXPECT_EQ(recs[3].header.recordKind, VN_MONITOR_RECORD_TAP_BINDING_CHANGED);
    EXPECT_EQ(recs[2].header.reason, VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE);
    EXPECT_EQ(recs[3].header.reason, VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT);

    const auto expected = ParseFile("registry.expected.json");
    const harkenhost::JsonValue* steps = RequireArray(expected, "steps");
    ASSERT_NE(steps, nullptr);
    ASSERT_EQ(steps->items.size(), 3u);
    EXPECT_EQ(RequireString(steps->items[0], "expectedResult"), "VN_OK");
    EXPECT_EQ(RequireString(steps->items[1], "expectedResult"), "VN_OK");
    EXPECT_EQ(RequireString(steps->items[2], "expectedResult"),
              "VN_MONITOR_STALE_TAP");
    EXPECT_EQ(RequireU64(steps->items[2], "sessionHandle"), 0u);
    EXPECT_EQ(VN_MONITOR_STALE_TAP, -338);
}

TEST(MonitorFixture, PlanMeasurementLayersStayIndependent) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorDecodedRecord> recs;
    DecodeCheckedIn("layers.bin", 5, &bytes, &recs);
    ASSERT_EQ(recs.size(), 5u);
    const auto expected = ParseFile("layers.expected.json");
    const harkenhost::JsonValue* layers = RequireArray(expected, "layers");
    ASSERT_NE(layers, nullptr);
    ASSERT_EQ(layers->items.size(), 5u);

    EXPECT_EQ(RequireString(layers->items[0], "capability"), "unsupported");
    EXPECT_EQ(RequireString(layers->items[0], "effectivePlan"), "not-requested");
    EXPECT_EQ(RequireString(layers->items[0], "validity"), "unsupported");
    EXPECT_EQ(RequireString(layers->items[0], "drop"), "none");
    EXPECT_EQ(recs[0].header.validityFlags, VN_MONITOR_VALID_UNSUPPORTED);
    EXPECT_EQ(recs[0].numericReadable, 0u);

    EXPECT_EQ(RequireString(layers->items[1], "capability"), "supported");
    EXPECT_EQ(RequireString(layers->items[1], "effectivePlan"), "not-armed");
    EXPECT_EQ(RequireString(layers->items[1], "validity"), "not-armed");
    EXPECT_EQ(recs[1].header.validityFlags, VN_MONITOR_VALID_NUMERIC);
    EXPECT_EQ(recs[1].numericReadable, 0u);

    EXPECT_EQ(RequireString(layers->items[2], "effectivePlan"), "armed");
    EXPECT_EQ(RequireString(layers->items[2], "validity"), "armed-warmup");
    EXPECT_EQ(recs[2].header.validityFlags,
              VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC |
                  VN_MONITOR_VALID_WARMUP);
    EXPECT_EQ(recs[2].numericReadable, 0u);

    EXPECT_EQ(RequireString(layers->items[3], "validity"), "armed-numeric");
    EXPECT_EQ(RequireString(layers->items[3], "drop"), "none");
    EXPECT_EQ(RequireU64(layers->items[3], "numericReadable"), 1u);
    EXPECT_EQ(recs[3].numericReadable, 1u);
    ASSERT_EQ(recs[3].payloadBytes, sizeof(VnMonitorTimeSeriesSample));
    const auto* sample =
        reinterpret_cast<const VnMonitorTimeSeriesSample*>(recs[3].payload);
    EXPECT_EQ(sample->valueBits, 0u);

    EXPECT_EQ(RequireString(layers->items[4], "drop"), "source-producer");
    EXPECT_EQ(recs[4].header.recordKind, VN_MONITOR_RECORD_DROP_SUMMARY);
    const auto* drop =
        reinterpret_cast<const VnMonitorDropSummary*>(recs[4].payload);
    EXPECT_EQ(drop->stage, VN_MONITOR_DROP_SOURCE_PRODUCER);
    EXPECT_EQ(drop->firstSourceSequence, 4u);
    EXPECT_EQ(drop->lastSourceSequence, 5u);
}

TEST(MonitorFixture, PlanLeaseStaleGenerationRejected) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorDecodedRecord> recs;
    DecodeCheckedIn("lease.bin", 2, &bytes, &recs);
    ASSERT_EQ(recs.size(), 2u);
    const auto expected = ParseFile("lease.expected.json");
    const harkenhost::JsonValue* steps = RequireArray(expected, "steps");
    ASSERT_NE(steps, nullptr);
    ASSERT_EQ(steps->items.size(), 4u);
    EXPECT_EQ(RequireU64(steps->items[0], "leaseGeneration"), 1u);
    EXPECT_EQ(RequireString(steps->items[1], "expectedResult"),
              "VN_MONITOR_STALE_LEASE");
    EXPECT_EQ(RequireU64(steps->items[1], "sessionHandle"), 0u);
    EXPECT_EQ(RequireU64(steps->items[2], "held"), 0u);
    EXPECT_EQ(RequireU64(steps->items[3], "leaseGeneration"), 2u);
    EXPECT_GT(RequireU64(steps->items[3], "leaseGeneration"),
              RequireU64(steps->items[0], "leaseGeneration"));
    EXPECT_EQ(VN_MONITOR_STALE_LEASE, -336);
}

TEST(MonitorFixture, PlanStopSequenceEmptyIdempotent) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorDecodedRecord> recs;
    DecodeCheckedIn("stop.bin", 1, &bytes, &recs);
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].header.recordKind, VN_MONITOR_RECORD_CAPTURE_PLAN_ACTIVATED);
    EXPECT_EQ(recs[0].header.capturePlanGeneration, 11u);
    EXPECT_EQ(recs[0].header.instanceGeneration, 10u);
    EXPECT_EQ(recs[0].header.reason, VN_MONITOR_EVENT_STOP);

    const auto expected = ParseFile("stop.expected.json");
    const auto* active = FindSequence(expected, "stop-active-to-empty");
    const auto* empty = FindSequence(expected, "stop-already-empty");
    const auto* stale = FindSequence(expected, "stop-stale-lease");
    const auto* full = FindSequence(expected, "stop-slots-full");
    ASSERT_NE(active, nullptr);
    ASSERT_NE(empty, nullptr);
    ASSERT_NE(stale, nullptr);
    ASSERT_NE(full, nullptr);

    const harkenhost::JsonValue* activeAfter = active->Find("after");
    const harkenhost::JsonValue* activeBefore = active->Find("before");
    const harkenhost::JsonValue* emptyAfter = empty->Find("after");
    const harkenhost::JsonValue* emptyBefore = empty->Find("before");
    const harkenhost::JsonValue* staleAfter = stale->Find("after");
    const harkenhost::JsonValue* staleBefore = stale->Find("before");
    const harkenhost::JsonValue* fullAfter = full->Find("after");
    const harkenhost::JsonValue* fullBefore = full->Find("before");
    ASSERT_NE(activeAfter, nullptr);
    ASSERT_NE(activeBefore, nullptr);
    ASSERT_NE(emptyAfter, nullptr);
    ASSERT_NE(emptyBefore, nullptr);
    ASSERT_NE(staleAfter, nullptr);
    ASSERT_NE(staleBefore, nullptr);
    ASSERT_NE(fullAfter, nullptr);
    ASSERT_NE(fullBefore, nullptr);

    EXPECT_EQ(RequireString(*activeAfter, "expectedResult"), "VN_OK");
    EXPECT_GT(RequireU64(*activeAfter, "capturePlanGeneration"),
              RequireU64(*activeBefore, "capturePlanGeneration"));
    EXPECT_GT(RequireU64(*activeAfter, "effectiveFromExecuteFrame"),
              RequireU64(*activeBefore, "effectiveFromExecuteFrame"));
    EXPECT_EQ(RequireU64(*activeAfter, "leaseGeneration"),
              RequireU64(*activeBefore, "leaseGeneration"));
    EXPECT_EQ(recs[0].header.executeFrame,
              RequireU64(*activeAfter, "effectiveFromExecuteFrame"));

    EXPECT_EQ(RequireString(*emptyAfter, "expectedResult"), "VN_OK");
    ExpectSameU64(*emptyBefore, *emptyAfter, "capturePlanGeneration");
    ExpectSameU64(*emptyBefore, *emptyAfter, "effectiveFromExecuteFrame");
    ExpectSameU64(*emptyBefore, *emptyAfter, "leaseGeneration");
    ExpectSameU64(*emptyBefore, *emptyAfter, "activationState");
    ExpectSameU64(*emptyBefore, *emptyAfter, "planEmpty");

    EXPECT_EQ(RequireString(*staleAfter, "expectedResult"),
              "VN_MONITOR_STALE_LEASE");
    ExpectSameU64(*staleBefore, *staleAfter, "capturePlanGeneration");
    ExpectSameU64(*staleBefore, *staleAfter, "effectiveFromExecuteFrame");
    ExpectSameU64(*staleBefore, *staleAfter, "activationState");
    ExpectSameU64(*staleBefore, *staleAfter, "planEmpty");
    ExpectSameU64(*staleBefore, *staleAfter, "leaseGeneration");

    EXPECT_EQ(RequireString(*fullAfter, "expectedResult"), "VN_OK");
    EXPECT_EQ(RequireU64(*fullAfter, "normalSlotsOccupied"), 8u);
    EXPECT_EQ(RequireU64(*fullAfter, "planEmpty"), 1u);
    EXPECT_GT(RequireU64(*fullAfter, "effectiveFromExecuteFrame"),
              RequireU64(*fullBefore, "effectiveFromExecuteFrame"));
    ExpectSameU64(*fullBefore, *fullAfter, "leaseGeneration");
}

TEST(MonitorFixture, PlanApplyFailureKeepsGeneration) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorDecodedRecord> recs;
    DecodeCheckedIn("apply_fail.bin", 1, &bytes, &recs);
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].header.capturePlanGeneration, 10u);

    const auto expected = ParseFile("apply_fail.expected.json");
    const auto* budget = FindSequence(expected, "apply-budget-fail");
    const auto* unsupported = FindSequence(expected, "apply-unsupported-fail");
    const auto* noLease = FindSequence(expected, "apply-no-lease");
    ASSERT_NE(budget, nullptr);
    ASSERT_NE(unsupported, nullptr);
    ASSERT_NE(noLease, nullptr);

    const harkenhost::JsonValue* budgetAfter = budget->Find("after");
    const harkenhost::JsonValue* budgetBefore = budget->Find("before");
    const harkenhost::JsonValue* unsupportedAfter = unsupported->Find("after");
    const harkenhost::JsonValue* unsupportedBefore = unsupported->Find("before");
    const harkenhost::JsonValue* noLeaseAfter = noLease->Find("after");
    const harkenhost::JsonValue* noLeaseBefore = noLease->Find("before");
    ASSERT_NE(budgetAfter, nullptr);
    ASSERT_NE(budgetBefore, nullptr);
    ASSERT_NE(unsupportedAfter, nullptr);
    ASSERT_NE(unsupportedBefore, nullptr);
    ASSERT_NE(noLeaseAfter, nullptr);
    ASSERT_NE(noLeaseBefore, nullptr);

    ExpectSameU64(*budgetBefore, *budgetAfter, "capturePlanGeneration");
    ExpectSameU64(*budgetBefore, *budgetAfter, "leaseGeneration");
    ExpectSameU64(*budgetBefore, *budgetAfter, "activationState");
    ExpectSameU64(*budgetBefore, *budgetAfter, "planEmpty");
    ExpectSameU64(*budgetBefore, *budgetAfter, "effectiveFromExecuteFrame");
    ExpectU64Array(*budgetBefore, "measurementIds", {1u, 2u});
    ExpectU64Array(*budgetAfter, "measurementIds", {1u, 2u});
    const harkenhost::JsonValue* accepted =
        RequireArray(*budgetAfter, "acceptedMeasurementIds");
    ASSERT_NE(accepted, nullptr);
    EXPECT_EQ(accepted->items.size(), 0u);
    EXPECT_EQ(RequireString(*budgetAfter, "expectedResult"),
              "VN_MONITOR_BUDGET_EXCEEDED");

    ExpectSameU64(*unsupportedBefore, *unsupportedAfter, "capturePlanGeneration");
    ExpectSameU64(*unsupportedBefore, *unsupportedAfter, "activationState");
    ExpectSameU64(*unsupportedBefore, *unsupportedAfter, "planEmpty");
    ExpectSameU64(*unsupportedBefore, *unsupportedAfter, "effectiveFromExecuteFrame");
    ExpectU64Array(*unsupportedAfter, "measurementIds", {1u});
    EXPECT_EQ(RequireArray(*unsupportedAfter, "acceptedMeasurementIds")->items.size(),
              0u);
    EXPECT_EQ(RequireString(*unsupportedAfter, "expectedResult"),
              "VN_MONITOR_UNSUPPORTED");

    ExpectSameU64(*noLeaseBefore, *noLeaseAfter, "capturePlanGeneration");
    ExpectSameU64(*noLeaseBefore, *noLeaseAfter, "leaseGeneration");
    ExpectSameU64(*noLeaseBefore, *noLeaseAfter, "activationState");
    ExpectSameU64(*noLeaseBefore, *noLeaseAfter, "planEmpty");
    ExpectSameU64(*noLeaseBefore, *noLeaseAfter, "effectiveFromExecuteFrame");
    EXPECT_EQ(RequireU64(*noLeaseAfter, "sessionHandle"), 0u);
    EXPECT_EQ(RequireString(*noLeaseAfter, "expectedResult"),
              "VN_MONITOR_PERMISSION_DENIED");
}
