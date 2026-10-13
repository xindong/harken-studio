#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "Common/TestUtil.h"
#include "harken/core/ActionFiringTable.h"
#include "harken/core/MonitorIdentity.h"
#include "harken/core/MonitorLifecycleSink.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

VnMonitorSessionDesc MakeLineageDesc() {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask =
        VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE;
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE;
    desc.consumerQueueBytes = 65536;
    return desc;
}

struct PollArgs {
    std::vector<unsigned char> buffer;
    VnMonitorPollDesc desc{};
    VnMonitorPollResult result{};

    explicit PollArgs(std::uint64_t bytes = 65536)
        : buffer(static_cast<std::size_t>(bytes)) {
        desc.structSize = sizeof(desc);
        desc.buffer = buffer.data();
        desc.bufferBytes = buffer.size();
        result.structSize = sizeof(result);
    }
};

std::uint32_t ReadU32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64(const unsigned char* p) {
    return static_cast<std::uint64_t>(ReadU32(p)) |
           (static_cast<std::uint64_t>(ReadU32(p + 4)) << 32);
}

struct PackedRecord {
    std::uint32_t recordKind = 0;
    std::uint32_t entityKind = 0;
    std::uint32_t reason = 0;
    std::uint32_t eventClass = 0;
    std::uint64_t recordBytes = 0;
    std::uint64_t worldGeneration = 0;
    std::uint64_t executeFrame = 0;
    std::uint64_t programSemanticFingerprint = 0;
    std::uint64_t runtimeLineageFingerprint = 0;
    std::uint64_t optionalSymbolFingerprint = 0;
    std::uint64_t instanceHandle = 0;
    std::uint64_t instanceGeneration = 0;
    std::uint64_t arrangingInstance = 0;
    std::uint32_t occurrenceOrdinal = 0;
    std::uint64_t sourceSequence = 0;
};

std::vector<PackedRecord> ParseRecords(const PollArgs& poll) {
    std::vector<PackedRecord> out;
    const unsigned char* bytes = poll.buffer.data();
    std::uint64_t offset = 0;
    const std::uint64_t total = poll.result.bytesWritten;
    const std::uint32_t envelope = VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
    while (offset + envelope <= total) {
        PackedRecord rec;
        rec.recordKind = ReadU32(bytes + offset);
        rec.recordBytes = ReadU64(bytes + offset + 16);
        rec.sourceSequence = ReadU64(bytes + offset + 40);
        rec.worldGeneration = ReadU64(bytes + offset + 72);
        rec.executeFrame = ReadU64(bytes + offset + 128);
        rec.entityKind = ReadU32(bytes + offset + 156);
        rec.programSemanticFingerprint = ReadU64(bytes + offset + 160);
        rec.runtimeLineageFingerprint = ReadU64(bytes + offset + 168);
        rec.optionalSymbolFingerprint = ReadU64(bytes + offset + 176);
        rec.instanceHandle = ReadU64(bytes + offset + 200);
        rec.instanceGeneration = ReadU64(bytes + offset + 208);
        rec.arrangingInstance = ReadU64(bytes + offset + 268);
        rec.occurrenceOrdinal = ReadU32(bytes + offset + 296);
        rec.reason = ReadU32(bytes + offset + 248);
        if (rec.recordBytes < envelope || offset + rec.recordBytes > total) {
            break;
        }
        if (rec.recordKind == VN_MONITOR_RECORD_EVENT &&
            rec.recordBytes >= envelope + sizeof(VnMonitorEventPayload)) {
            VnMonitorEventPayload payload{};
            std::memcpy(&payload, bytes + offset + envelope, sizeof(payload));
            rec.eventClass = payload.eventClass;
        }
        if (rec.recordKind == VN_MONITOR_RECORD_DROP_SUMMARY &&
            rec.recordBytes >= envelope + sizeof(VnMonitorDropSummary)) {
            VnMonitorDropSummary payload{};
            std::memcpy(&payload, bytes + offset + envelope, sizeof(payload));
            rec.reason = payload.reason;
        }
        out.push_back(rec);
        offset += rec.recordBytes;
    }
    return out;
}

std::uint32_t CountKind(const std::vector<PackedRecord>& records,
                        std::uint32_t recordKind, std::uint32_t eventClass,
                        std::uint32_t entityKind) {
    std::uint32_t count = 0;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind == recordKind && rec.entityKind == entityKind &&
            (eventClass == 0 || rec.eventClass == eventClass)) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST(MonitorObserve, LineageHelperKeepsMetadataOffTheInstanceKey) {
    using harken::MonitorIdentityFacts;
    using harken::MonitorWorldAvailability;
    MonitorIdentityFacts previous{};
    previous.key.handle = 11;
    previous.key.generation = 2;
    previous.worldAvailability = MonitorWorldAvailability::Known;
    previous.world = 7;
    previous.programSemanticFingerprint = 17;
    previous.runtimeLineageFingerprint = 19;
    previous.optionalSymbolFingerprint = 23;
    previous.renderPlanGeneration = 29;
    previous.clockAnchorGeneration = 31;
    MonitorIdentityFacts lineageNext = previous;
    lineageNext.runtimeLineageFingerprint = 41;
    const auto lineageOnly =
        harken::ClassifyMonitorIdentityChange(previous, lineageNext);
    EXPECT_FALSE(lineageOnly.replaceInstanceKey);
    EXPECT_FALSE(lineageOnly.updateProgramFingerprint);
    EXPECT_TRUE(lineageOnly.updateLineageFingerprint);
    EXPECT_FALSE(lineageOnly.updateSymbolFingerprint);
    EXPECT_FALSE(lineageOnly.updateRenderPlanGeneration);
    EXPECT_FALSE(lineageOnly.updateClockAnchorGeneration);

    MonitorIdentityFacts symbolNext = previous;
    symbolNext.optionalSymbolFingerprint = 43;
    const auto symbolOnly =
        harken::ClassifyMonitorIdentityChange(previous, symbolNext);
    EXPECT_FALSE(symbolOnly.replaceInstanceKey);
    EXPECT_FALSE(symbolOnly.updateProgramFingerprint);
    EXPECT_FALSE(symbolOnly.updateLineageFingerprint);
    EXPECT_TRUE(symbolOnly.updateSymbolFingerprint);
    EXPECT_FALSE(symbolOnly.updateRenderPlanGeneration);
    EXPECT_FALSE(symbolOnly.updateClockAnchorGeneration);

    MonitorIdentityFacts renderClockNext = previous;
    renderClockNext.renderPlanGeneration = 47;
    renderClockNext.clockAnchorGeneration = 53;
    const auto renderClockOnly =
        harken::ClassifyMonitorIdentityChange(previous, renderClockNext);
    EXPECT_FALSE(renderClockOnly.replaceInstanceKey);
    EXPECT_FALSE(renderClockOnly.updateProgramFingerprint);
    EXPECT_FALSE(renderClockOnly.updateLineageFingerprint);
    EXPECT_FALSE(renderClockOnly.updateSymbolFingerprint);
    EXPECT_TRUE(renderClockOnly.updateRenderPlanGeneration);
    EXPECT_TRUE(renderClockOnly.updateClockAnchorGeneration);

    MonitorIdentityFacts worldNext = previous;
    worldNext.world = 8;
    worldNext.key.handle = 13;
    worldNext.key.generation = 3;
    const auto worldChange =
        harken::ClassifyMonitorIdentityChange(previous, worldNext);
    EXPECT_TRUE(worldChange.replaceInstanceKey);

    MonitorIdentityFacts unavailable{};
    unavailable.worldAvailability = MonitorWorldAvailability::Unavailable;
    unavailable.world = 0;
    unavailable.key = previous.key;
    MonitorIdentityFacts unavailableNext = unavailable;
    unavailableNext.world = 9;
    const auto keepKey =
        harken::ClassifyMonitorIdentityChange(unavailable, unavailableNext);
    EXPECT_FALSE(keepKey.replaceInstanceKey);
    EXPECT_EQ(1u, harken::kMonitorIdentityHelperVersion);
}

TEST(MonitorObserve, LineageActionFingerprintReachesPollWithoutSymbols) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeLineageDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::ActionFiringTable actions;
    ASSERT_TRUE(actions.Reset(1));
    harken::PreparedActionFiring prepared{};
    ASSERT_TRUE(actions.PrepareRoot(harkentest::ObjectId(41), 7, 0, &prepared));
    const harken::ActionFiringHandle handle = prepared.Handle();
    ASSERT_TRUE(actions.Commit(&prepared, 99));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    bool found = false;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind != VN_MONITOR_RECORD_EVENT ||
            rec.entityKind != VN_MONITOR_ENTITY_ACTION ||
            rec.eventClass != VN_MONITOR_EVENT_ACCEPT) {
            continue;
        }
        found = true;
        EXPECT_EQ(handle, rec.instanceHandle);
        EXPECT_NE(0u, rec.instanceGeneration);
        EXPECT_EQ(0u, rec.programSemanticFingerprint);
        EXPECT_EQ(0u, rec.optionalSymbolFingerprint);
        EXPECT_EQ(0u, rec.runtimeLineageFingerprint);
        EXPECT_EQ(7u, rec.worldGeneration);
        EXPECT_EQ(0u, rec.arrangingInstance);
    }
    EXPECT_TRUE(found);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, LineageLateWorldStateDoesNotEnterCurrentView) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();
    harken::monitor::SetCurrentWorldGenerationForTests(8);

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeLineageDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::ActionFiringTable actions;
    ASSERT_TRUE(actions.Reset(1));
    harken::PreparedActionFiring prepared{};
    ASSERT_TRUE(actions.PrepareRoot(harkentest::ObjectId(41), 7, 0, &prepared));
    ASSERT_TRUE(actions.Commit(&prepared, 99));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_ACCEPT,
                        VN_MONITOR_ENTITY_ACTION),
              1u);
    EXPECT_EQ(0u, CountKind(records, VN_MONITOR_RECORD_STATE_UPSERT, 0,
                            VN_MONITOR_ENTITY_ACTION));
    bool gap = false;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind == VN_MONITOR_RECORD_DROP_SUMMARY &&
            rec.reason == VN_ACTION_TERMINAL_WORLD_REPLACED) {
            gap = true;
        }
    }
    EXPECT_TRUE(gap);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
    harken::monitor::SetCurrentWorldGenerationForTests(0);
}

TEST(MonitorObserve, LineageWideOccurrenceStaysZeroInPublishedOrdinal) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeLineageDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::ObserveMonitorLifecycle(
        harken::MonitorLifecyclePhase::QueueAdvance, 11, 2, 7, harkentest::ObjectId(13), 11, 0, 0, 0,
        5ull);
    harken::ObserveMonitorLifecycle(
        harken::MonitorLifecyclePhase::QueueAdvance, 13, 2, 7, harkentest::ObjectId(13), 13, 0, 0, 0,
        0x100000001ull);
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    bool foundNarrow = false;
    bool foundWide = false;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind != VN_MONITOR_RECORD_EVENT ||
            rec.entityKind != VN_MONITOR_ENTITY_QUEUE ||
            rec.eventClass != VN_MONITOR_EVENT_QUEUE) {
            continue;
        }
        if (rec.instanceHandle == 11) {
            foundNarrow = true;
            EXPECT_EQ(5u, rec.occurrenceOrdinal);
        }
        if (rec.instanceHandle == 13) {
            foundWide = true;
            EXPECT_EQ(0u, rec.occurrenceOrdinal);
        }
    }
    EXPECT_TRUE(foundNarrow);
    EXPECT_TRUE(foundWide);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}
