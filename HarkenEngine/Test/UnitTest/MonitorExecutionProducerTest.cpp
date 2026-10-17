#include "MonitorIdentityState.h"
#include "MonitorExecutionProducer.h"
#include "MonitorExecutionCodec.h"
#include "MonitorCaptureHub.h"
#include "MonitorCaptureHubTestHooks.h"
#include "Common/TestUtil.h"
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace {

using namespace harken;
using namespace harken::monitor;

MonitorExecutionFact Source(std::uint64_t engine = 19) {
    MonitorExecutionFact source{};
    source.engine = engine;
    source.kind = VN_MONITOR_FACT_REQUEST_SUBMITTED;
    source.flags = kMonitorFactCallBody;
    source.body.call = {};
    source.timestamp = 1;
    SetSourceField(source, VN_MONITOR_FACT_FIELD_TIMESTAMP);
    return source;
}

TEST(MonitorExecutionProducer, AccountsFullRingDropsOnlyAfterProjectionAndEmptyRead) {
    MonitorProducerRing ring;
    ring.Init(1, sizeof(MonitorExecutionFact));
    MonitorExecutionProducer producer;
    ASSERT_TRUE(producer.Publish(ring, Source(), 19));
    ASSERT_FALSE(producer.Publish(ring, Source(), 19));
    EXPECT_EQ(2u, producer.Published());
    EXPECT_EQ(0u, producer.Completed());
    std::array<std::uint8_t, sizeof(MonitorExecutionFact)> bytes{};
    std::uint32_t size = 0, kind = 0;
    ASSERT_TRUE(ring.TryPop(bytes.data(), bytes.size(), &size, &kind));
    ASSERT_EQ(kExecutionSourceKind, kind);
    ASSERT_TRUE(producer.Stage(bytes.data(), size));
    std::uint64_t dropped = 0;
    const auto drop = [&](auto first, auto last) { dropped += last - first + 1; };
    ASSERT_TRUE(producer.DispatchPending([&](const auto& fact) {
        EXPECT_EQ(1u, fact.sourceSequence);
        EXPECT_EQ(0u, producer.Completed());
        return true;
    }, drop));
    EXPECT_EQ(1u, producer.Completed());
    EXPECT_EQ(0u, dropped);
    const auto cutoff = producer.Published();
    ASSERT_FALSE(ring.TryPop(bytes.data(), bytes.size(), &size, &kind));
    ASSERT_TRUE(producer.FinishEmpty(cutoff, drop));
    EXPECT_EQ(2u, producer.Completed());
    EXPECT_EQ(1u, dropped);
}

TEST(MonitorExecutionProducer, DeclaredSourceLossUsesTheSameSequenceAndCompletionFence) {
    MonitorProducerRing ring;
    ring.Init(1, sizeof(MonitorExecutionFact));
    MonitorExecutionProducer producer;
    ASSERT_TRUE(producer.Publish(ring, Source(), 19));
    producer.DeclareDrop(19);
    ASSERT_FALSE(producer.Publish(ring, Source(), 19));
    EXPECT_EQ(3u, producer.Published());
    EXPECT_EQ(0u, producer.Completed());
    std::array<std::uint8_t, sizeof(MonitorExecutionFact)> bytes{};
    std::uint32_t size = 0, kind = 0;
    ASSERT_TRUE(ring.TryPop(bytes.data(), bytes.size(), &size, &kind));
    ASSERT_TRUE(producer.Stage(bytes.data(), size));
    std::uint64_t lost = 0;
    const auto drop = [&](auto first, auto last) {
        EXPECT_EQ(2u, first);
        EXPECT_EQ(3u, last);
        lost += last - first + 1;
    };
    ASSERT_TRUE(producer.DispatchPending([](const auto&) { return true; }, drop));
    EXPECT_EQ(0u, lost);
    const auto cutoff = producer.Published();
    ASSERT_FALSE(ring.TryPop(bytes.data(), bytes.size(), &size, &kind));
    ASSERT_TRUE(producer.FinishEmpty(cutoff, drop));
    EXPECT_EQ(2u, lost);
    EXPECT_EQ(3u, producer.Completed());
    producer.Reset(UINT64_MAX - 1);
    producer.DeclareDrop(19);
    producer.DeclareDrop(19);
    producer.DeclareDrop(0);
    EXPECT_EQ(UINT64_MAX, producer.Published());
    EXPECT_EQ(2u, producer.Unavailable());
}

TEST(MonitorExecutionProducer, RetainsSlotUntilItsPublicationIsComplete) {
    MonitorExecutionProducer producer;
    auto pending = Source();
    pending.sourceSequence = 1;
    ASSERT_TRUE(producer.Stage(reinterpret_cast<const std::uint8_t*>(&pending), sizeof(pending)));
    bool dispatched = false;
    const auto consume = [&](const auto&) { dispatched = true; return true; };
    const auto drop = [](auto, auto) { ADD_FAILURE(); };
    EXPECT_FALSE(producer.DispatchPending(consume, drop));
    EXPECT_FALSE(producer.FinishEmpty(0, drop));
    EXPECT_FALSE(dispatched);
    MonitorProducerRing ring;
    ring.Init(1, sizeof(MonitorExecutionFact));
    ASSERT_TRUE(producer.Publish(ring, Source(), 19));
    EXPECT_TRUE(producer.DispatchPending(consume, drop));
    EXPECT_TRUE(dispatched);
    EXPECT_EQ(1u, producer.Completed());
}

TEST(MonitorExecutionProducer, SourceSequenceDoesNotWrapAndForeignEngineIsUnavailable) {
    MonitorProducerRing ring;
    ring.Init(2, sizeof(MonitorExecutionFact));
    MonitorExecutionProducer producer;
    producer.Reset(UINT64_MAX - 1);
    ASSERT_TRUE(producer.Publish(ring, Source(), 19));
    EXPECT_EQ(UINT64_MAX, producer.Published());
    EXPECT_FALSE(producer.Publish(ring, Source(), 19));
    EXPECT_EQ(UINT64_MAX, producer.Published());
    EXPECT_EQ(1u, producer.Unavailable());
    producer.Reset();
    EXPECT_FALSE(producer.Publish(ring, Source(20), 19));
    EXPECT_EQ(0u, producer.Published());
    EXPECT_EQ(1u, producer.Unavailable());
}

TEST(MonitorExecutionProducer, ConcurrentSpscAccountingCoversEveryCompletedAttemptOnce) {
    MonitorProducerRing ring;
    ring.Init(8, sizeof(MonitorExecutionFact));
    MonitorExecutionProducer producer;
    constexpr std::uint64_t attempts = 10000;
    std::atomic<bool> finished{false};
    std::thread writer([&] {
        for (std::uint64_t i = 0; i != attempts; ++i) producer.Publish(ring, Source(), 19);
        finished.store(true, std::memory_order_release);
    });
    std::uint64_t facts = 0, drops = 0, lastFact = 0;
    std::array<std::uint8_t, sizeof(MonitorExecutionFact)> bytes{};
    const auto consume = [&](const auto& fact) {
        EXPECT_GT(fact.sourceSequence, lastFact);
        lastFact = fact.sourceSequence;
        ++facts;
        return true;
    };
    const auto drop = [&](auto first, auto last) { drops += last - first + 1; };
    while (!finished.load(std::memory_order_acquire) || producer.Completed() != producer.Published()) {
        if (producer.HasPending()) { producer.DispatchPending(consume, drop); continue; }
        const auto cutoff = producer.Published();
        std::uint32_t size = 0;
        if (ring.TryPop(bytes.data(), bytes.size(), &size)) {
            EXPECT_TRUE(producer.Stage(bytes.data(), size));
            producer.DispatchPending(consume, drop);
        } else producer.FinishEmpty(cutoff, drop);
    }
    writer.join();
    EXPECT_EQ(attempts, facts + drops);
    EXPECT_EQ(attempts, producer.Completed());
    EXPECT_EQ(0u, producer.Unavailable());
}

VnMonitorSessionHandle Open(bool typed = true, bool catalog = true, std::uint32_t budget = 65536) {
    VnMonitorSessionDescV2 desc{};
    desc.structSize = typed ? sizeof(desc) : sizeof(VnMonitorSessionDesc);
    desc.requestedExecutionFactVersion = typed ? 1 : 0;
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    desc.consumerQueueBytes = budget;
    VnMonitorSessionHandle session = 0;
    const auto result = catalog ? Vn_MonitorOpenEventSession(reinterpret_cast<const VnMonitorSessionDesc*>(&desc), &session)
        : Vn_MonitorOpenSession(reinterpret_cast<const VnMonitorSessionDesc*>(&desc), &session);
    EXPECT_EQ(VN_OK, result);
    return session;
}

VnMonitorEventSessionStatusV2 Status(VnMonitorSessionHandle session) {
    VnMonitorEventSessionStatusV2 status{};
    status.structSize = sizeof(status);
    EXPECT_EQ(VN_OK, Vn_MonitorGetEventSessionStatus(session, reinterpret_cast<VnMonitorEventSessionStatus*>(&status)));
    return status;
}

std::uint64_t Read64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return value;
}

struct FactKey { std::uint64_t producer, sequence, engine, timestamp; };

std::vector<FactKey> PollFacts(VnMonitorSessionHandle session, std::vector<VnMonitorDropSummary>* drops = nullptr) {
    std::array<std::uint8_t, 65536> bytes{};
    VnMonitorPollDesc desc{};
    desc.structSize = sizeof(desc);
    desc.buffer = bytes.data();
    desc.bufferBytes = bytes.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    EXPECT_EQ(VN_OK, Vn_MonitorPoll(session, &desc, &result));
    std::vector<FactKey> facts;
    for (std::uint64_t at = 0; at < result.bytesWritten;) {
        const auto size = Read64(bytes.data() + at + 16);
        if (size < kPackedEnvelopeBytes || size > result.bytesWritten - at) { ADD_FAILURE(); break; }
        if (bytes[at] == VN_MONITOR_RECORD_EXECUTION_FACT) facts.push_back({Read64(bytes.data() + at + 32),
            Read64(bytes.data() + at + 40), Read64(bytes.data() + at + 56), Read64(bytes.data() + at + kPackedEnvelopeBytes + 24)});
        if (drops != nullptr && bytes[at] == VN_MONITOR_RECORD_DROP_SUMMARY && size >= PackedDropSummaryBytes()) {
            VnMonitorDropSummary summary{};
            std::memcpy(&summary, bytes.data() + at + kPackedEnvelopeBytes, sizeof(summary));
            drops->push_back(summary);
        }
        at += size;
    }
    return facts;
}

TEST(MonitorExecutionHub, SealWaitsForBothProducersAndTheConsumerKeepsLateTimestamp) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto session = Open();
    StopDrainForTests();
    const auto engineId = ExecutionEngineInstance();
    ASSERT_NE(0u, engineId);
    for (unsigned i = 0; i != 5; ++i) ASSERT_TRUE(PublishExecutionFact(Source(engineId)));
    ASSERT_TRUE(PublishExecutionFact(Source(engineId), true));
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(session));
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_SEALED, Status(session).state);
    DrainOneProducerForTests(false);
    EXPECT_EQ(5u, PollFacts(session).size());
    // Logic completed sequence 5 does not prove audio sequence 1 completed.
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_SEALED, Status(session).state);
    DrainOneProducerForTests(true);
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_SEALED, Status(session).state);
    const auto facts = PollFacts(session);
    ASSERT_EQ(1u, facts.size());
    EXPECT_EQ(kExecutionAudioProducer, facts[0].producer);
    EXPECT_EQ(1u, facts[0].sequence);
    EXPECT_EQ(1u, facts[0].timestamp);
    EXPECT_EQ(engineId, facts[0].engine);
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, Status(session).state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionHub, ShortLogicFactBurstFitsTheExistingByteBudgetBeforeDrain) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto session = Open();
    StopDrainForTests();
    const auto capacity = LogicRingCapacityForTests();
    const auto slotBytes = PackedLogicProducerSlotBytes();
    EXPECT_LE(static_cast<std::uint64_t>(capacity) *
        (slotBytes + MonitorProducerRing::kSlotMetadataBytes), VN_MONITOR_DEFAULT_PRODUCER_RING_BYTES);
    EXPECT_LE(sizeof(MonitorExecutionFact), slotBytes);
    testing::Test::RecordProperty("logic_producer_slots", std::to_string(capacity));
    testing::Test::RecordProperty("logic_producer_slot_bytes", std::to_string(slotBytes));
    constexpr unsigned count = 32;
    const auto source = Source(ExecutionEngineInstance());
    for (unsigned i = 0; i != count; ++i) ASSERT_TRUE(PublishExecutionFact(source));
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(session));
    DrainOnceForTests();
    const auto facts = PollFacts(session);
    ASSERT_EQ(count, facts.size());
    for (unsigned i = 0; i != count; ++i) EXPECT_EQ(i + 1, facts[i].sequence);
    EXPECT_EQ(0u, Status(session).sourceDroppedRecords);
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, Status(session).state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionHub, CaptureWindowsUsePublicationInsteadOfFanoutTime) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto first = Open();
    StopDrainForTests();
    const auto source = Source(ExecutionEngineInstance());
    ASSERT_TRUE(PublishExecutionFact(source));
    const auto second = Open();
    ASSERT_TRUE(PublishExecutionFact(source));
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(first));
    ASSERT_TRUE(PublishExecutionFact(source));
    DrainOnceForTests();
    const auto a = PollFacts(first);
    const auto b = PollFacts(second);
    ASSERT_EQ(2u, a.size());
    ASSERT_EQ(2u, b.size());
    EXPECT_EQ(1u, a[0].sequence);
    EXPECT_EQ(2u, a[1].sequence);
    EXPECT_EQ(2u, b[0].sequence);
    EXPECT_EQ(3u, b[1].sequence);
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, Status(first).state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(first));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(second));
}

TEST(MonitorExecutionHub, FullRingLossIsClippedToEachCaptureWindow) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto first = Open();
    StopDrainForTests();
    ASSERT_TRUE(FillLogicRingForTests());
    const auto source = Source(ExecutionEngineInstance());
    EXPECT_FALSE(PublishExecutionFact(source));
    const auto second = Open();
    EXPECT_FALSE(PublishExecutionFact(source));
    EXPECT_EQ(VN_OK, Vn_MonitorSealEventSession(first));
    EXPECT_FALSE(PublishExecutionFact(source));
    DrainOnceForTests();
    EXPECT_TRUE(PollFacts(first).empty());
    EXPECT_TRUE(PollFacts(second).empty());
    EXPECT_EQ(2u, Status(first).sourceDroppedRecords);
    EXPECT_EQ(2u, Status(second).sourceDroppedRecords);
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, Status(first).state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(first));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(second));
}

TEST(MonitorExecutionHub, LegacySessionsAndInternalRecorderDoNotReceiveTypedRecords) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto legacy = Open(false);
    StopDrainForTests();
    const auto ordinary = Open(true, false);
    // Opening an ordinary session can enqueue legacy availability records.
    // Finish that work before measuring the typed publication in isolation.
    DrainOnceForTests();
    EnableInternalConsumersForTests(1);
    const auto before = InternalQueuedCountForTests(0);
    ASSERT_TRUE(PublishExecutionFact(Source(ExecutionEngineInstance())));
    DrainOnceForTests();
    EXPECT_TRUE(PollFacts(legacy).empty());
    EXPECT_EQ(1u, PollFacts(ordinary).size());
    EXPECT_EQ(before, InternalQueuedCountForTests(0));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(legacy));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(ordinary));
}

TEST(MonitorExecutionHub, InactiveCaptureDoesNotPublishAndForeignEngineIsReported) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto engineId = ExecutionEngineInstance();
    EXPECT_FALSE(ExecutionObservationEnabled());
    EXPECT_FALSE(PublishExecutionFact(Source(engineId)));
    const auto session = Open();
    StopDrainForTests();
    EXPECT_FALSE(PublishExecutionFact(Source(engineId + 1)));
    EXPECT_EQ(1u, Status(session).identityUnavailableCount);
    EXPECT_TRUE(PublishExecutionFact(Source(engineId)));
    DrainOnceForTests();
    const auto facts = PollFacts(session);
    ASSERT_EQ(1u, facts.size());
    EXPECT_EQ(1u, facts[0].sequence);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
    EXPECT_FALSE(ExecutionObservationEnabled());
}

TEST(MonitorExecutionHub, DeliveryDropsKeepIndependentProducerRangesAndFlushBeforeSeal) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto session = Open(true, true, 1600);
    StopDrainForTests();
    const auto source = Source(ExecutionEngineInstance());
    for (unsigned i = 0; i != 3; ++i) ASSERT_TRUE(PublishExecutionFact(source));
    ASSERT_TRUE(PublishExecutionFact(source, true));
    // This tests per-source drop ranges independently of inter-source scheduling.
    DrainOneProducerForTests(false);
    DrainOneProducerForTests(true);
    std::array<VnMonitorDropSummary, 6> summaries{};
    std::uint32_t count = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorGetConsumerStatus(session, summaries.data(), summaries.size(), &count));
    ASSERT_EQ(2u, count);
    EXPECT_EQ(kExecutionLogicStream, summaries[0].streamId);
    EXPECT_EQ(3u, summaries[0].firstSourceSequence);
    EXPECT_EQ(3u, summaries[0].lastSourceSequence);
    EXPECT_EQ(kExecutionAudioStream, summaries[1].streamId);
    EXPECT_EQ(1u, summaries[1].firstSourceSequence);
    EXPECT_EQ(1u, summaries[1].lastSourceSequence);
    for (unsigned i = 0; i != count; ++i) {
        EXPECT_EQ(VN_MONITOR_DROP_HUB_FANOUT, summaries[i].stage);
        EXPECT_EQ(1u, summaries[i].recordCount);
        EXPECT_EQ(kExecutionRecordBytes, summaries[i].byteCount);
    }
    EXPECT_EQ(2u, PollFacts(session).size());
    ASSERT_TRUE(PublishExecutionFact(source));
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(session));
    DrainOnceForTests();
    std::vector<VnMonitorDropSummary> delivered;
    const auto facts = PollFacts(session, &delivered);
    ASSERT_EQ(1u, facts.size());
    EXPECT_EQ(4u, facts[0].sequence);
    ASSERT_EQ(2u, delivered.size());
    EXPECT_EQ(kExecutionLogicStream, delivered[0].streamId);
    EXPECT_EQ(3u, delivered[0].firstSourceSequence);
    EXPECT_EQ(3u, delivered[0].lastSourceSequence);
    EXPECT_EQ(kExecutionAudioStream, delivered[1].streamId);
    EXPECT_EQ(1u, delivered[1].firstSourceSequence);
    EXPECT_EQ(1u, delivered[1].lastSourceSequence);
    EXPECT_EQ(2u, Status(session).deliveryDroppedRecords);
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, Status(session).state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionHub, OrdinaryV2SourceLossRemainsQueryableWhenItsSummaryIsAlsoLost) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = Open(true, false, 1);
    StopDrainForTests();
    ASSERT_TRUE(FillLogicRingForTests());
    for (unsigned i = 0; i != 3; ++i) EXPECT_FALSE(PublishExecutionFact(Source(ExecutionEngineInstance())));
    DrainOnceForTests();
    EXPECT_TRUE(PollFacts(session).empty());
    std::uint32_t required = 0;
    EXPECT_EQ(VN_MONITOR_BUFFER_TOO_SMALL, Vn_MonitorGetConsumerStatus(session, nullptr, 0, &required));
    ASSERT_GE(required, 2u);
    ASSERT_LE(required, 6u);
    for (unsigned query = 0; query != 2; ++query) {
        std::array<VnMonitorDropSummary, 6> summaries{};
        ASSERT_EQ(VN_OK, Vn_MonitorGetConsumerStatus(session, summaries.data(), summaries.size(), &required));
        unsigned found = 0;
        for (unsigned i = 0; i != required; ++i) {
            if (summaries[i].stage != VN_MONITOR_DROP_SOURCE_PRODUCER || summaries[i].streamId != kExecutionLogicStream) continue;
            ++found;
            EXPECT_EQ(1u, summaries[i].firstSourceSequence);
            EXPECT_EQ(3u, summaries[i].lastSourceSequence);
            EXPECT_EQ(3u, summaries[i].recordCount);
            EXPECT_EQ(3u * kExecutionRecordBytes, summaries[i].byteCount);
        }
        EXPECT_EQ(1u, found);
    }
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
    const auto reopened = Open(true, false);
    std::array<VnMonitorDropSummary, 6> summaries{};
    ASSERT_EQ(VN_OK, Vn_MonitorGetConsumerStatus(reopened, summaries.data(), summaries.size(), &required));
    EXPECT_EQ(0u, required);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(reopened));
}

TEST(MonitorExecutionHub, CoveredLegacySourceDropsDoNotPolluteExecutionSession) {
    for (const bool legacyAlongside : {false, true}) {
        harkentest::EngineSession engine(harkentest::MakeNullConfig());
        ASSERT_EQ(VN_OK, engine.InitResult());
        ASSERT_EQ(VN_OK, Vn_Update());
        const auto typed = Open(true, true);
        const auto legacy = legacyAlongside ? Open(false, true) : 0;
        StopDrainForTests();
        ASSERT_TRUE(FillLogicRingForTests());
        ASSERT_EQ(VN_OK, Vn_StopAll(0));
        DrainOnceForTests();
        EXPECT_EQ(2u, Status(typed).sourceDroppedRecords);
        if (legacy != 0) {
            EXPECT_EQ(1u, Status(legacy).sourceDroppedRecords);
            EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(legacy));
        }
        EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(typed));
    }
}

} // namespace

namespace {
MonitorExecutionFact IdentityValue(std::uint64_t handle, std::uint64_t content = 0) {
    MonitorExecutionFact value{};
    value.engine = 7;
    value.kind = VN_MONITOR_FACT_IDENTITY_UPSERT;
    value.subjectNamespace = VN_MONITOR_SUBJECT_PLAYABLE;
    value.body.execution.identity.handle = handle;
    harken::monitor::SetSourceField(value, VN_MONITOR_FACT_FIELD_SUBJECT);
    if (content != 0) {
        value.body.execution.identity.object = {0, content};
        harken::monitor::SetSourceField(value, VN_MONITOR_FACT_FIELD_CONTENT_OBJECT);
    }
    return value;
}

TEST(MonitorIdentityState, UnchangedBoundariesStaySilentAndReusePreservesOldValues) {
    using namespace harken::monitor;
    MonitorIdentityMirror mirror;
    mirror.Reset(2);
    std::array<MonitorExecutionFact, 2> native{};
    std::vector<MonitorIdentityDelta> changes;
    const auto capture = [&](std::size_t slot, MonitorExecutionFact& out) {
        out = native[slot];
        return out.engine != 0;
    };
    const auto publish = [&](const MonitorExecutionFact& fact, std::uint64_t revision) {
        changes.push_back({fact.engine, 0, revision, fact});
    };
    ASSERT_TRUE(mirror.Refresh(capture, publish));
    EXPECT_TRUE(changes.empty());
    native[0] = IdentityValue(UINT64_C(1) << 32, 10);
    ASSERT_TRUE(mirror.Refresh(capture, publish));
    ASSERT_EQ(1u, changes.size());
    const auto firstRevision = changes.back().revision;
    ASSERT_TRUE(mirror.Refresh(capture, publish));
    EXPECT_EQ(1u, changes.size());
    native[0].timestamp = 99;
    native[0].sourceSequence = 42;
    SetSourceField(native[0], VN_MONITOR_FACT_FIELD_TIMESTAMP);
    ASSERT_TRUE(mirror.Refresh(capture, publish));
    EXPECT_EQ(1u, changes.size());
    native[0] = IdentityValue(UINT64_C(2) << 32, 20);
    ASSERT_TRUE(mirror.Refresh(capture, publish));
    ASSERT_EQ(3u, changes.size());
    EXPECT_EQ(VN_MONITOR_FACT_IDENTITY_TOMBSTONE, changes[1].fact.kind);
    EXPECT_EQ((VnObjectId{0, 10}), changes[1].fact.body.execution.identity.object);
    EXPECT_EQ(UINT64_C(1) << 32, changes[1].fact.body.execution.identity.handle);
    EXPECT_EQ(VN_MONITOR_FACT_IDENTITY_UPSERT, changes[2].fact.kind);
    EXPECT_EQ(UINT64_C(2) << 32, changes[2].fact.body.execution.identity.handle);
    EXPECT_EQ(changes[1].revision, changes[2].revision);
    EXPECT_GT(changes[2].revision, firstRevision);
    std::array<MonitorExecutionFact, 2> frozen{};
    std::uint64_t counts[9]{};
    std::uint32_t copied = 0;
    ASSERT_TRUE(mirror.Copy(frozen, copied, counts));
    ASSERT_EQ(1u, copied);
    EXPECT_EQ(1u, counts[VN_MONITOR_SUBJECT_PLAYABLE]);
    native[0] = {};
    ASSERT_TRUE(mirror.Refresh(capture, publish));
    ASSERT_EQ(4u, changes.size());
    EXPECT_EQ(VN_MONITOR_FACT_IDENTITY_TOMBSTONE, changes[3].fact.kind);
    EXPECT_EQ(UINT64_C(2) << 32, frozen[0].body.execution.identity.handle);
    EXPECT_TRUE(mirror.Copy(frozen, copied, counts));
    EXPECT_EQ(0u, copied);
}

TEST(MonitorIdentityState, CopyReportsFullCountsWhenStorageIsTooSmall) {
    using namespace harken::monitor;
    MonitorIdentityMirror mirror;
    mirror.Reset(3);
    ASSERT_TRUE(mirror.Refresh([](std::size_t slot, MonitorExecutionFact& out) {
        out = IdentityValue((UINT64_C(1) << 32) | slot);
        return true;
    }, [](const auto&, auto) {}));
    std::array<MonitorExecutionFact, 1> rows{};
    std::uint64_t counts[9]{};
    std::uint32_t copied = 0;
    EXPECT_FALSE(mirror.Copy(rows, copied, counts));
    EXPECT_EQ(1u, copied);
    EXPECT_EQ(3u, counts[VN_MONITOR_SUBJECT_PLAYABLE]);
}

TEST(MonitorIdentityState, StateOverflowHasItsOwnPublishedAndCompletedWindow) {
    using namespace harken::monitor;
    MonitorProducerRing stateRing, eventRing;
    stateRing.Init(1, sizeof(MonitorIdentityDelta));
    eventRing.Init(1, sizeof(MonitorExecutionFact));
    MonitorIdentityProducer state;
    MonitorExecutionProducer events;
    auto identity = IdentityValue(UINT64_C(1) << 32);
    EXPECT_TRUE(state.Publish(stateRing, {7, 0, 2, identity}, 7));
    EXPECT_FALSE(state.Publish(stateRing, {7, 0, 3, identity}, 7));
    EXPECT_EQ(2u, state.Published());
    EXPECT_EQ(0u, events.Published());
    identity.kind = VN_MONITOR_FACT_PLAYABLE_ACTIVE;
    EXPECT_TRUE(events.Publish(eventRing, identity, 7));
    std::array<std::uint8_t, sizeof(MonitorIdentityDelta)> bytes{};
    std::uint32_t size = 0;
    ASSERT_TRUE(stateRing.TryPop(bytes.data(), bytes.size(), &size));
    ASSERT_TRUE(state.Stage(bytes.data(), size));
    std::uint64_t lost = 0, revision = 0;
    const auto drop = [&](auto first, auto last) { lost += last - first + 1; };
    ASSERT_TRUE(state.DispatchPending([&](const MonitorIdentityDelta& delta) {
        revision = delta.revision;
        return true;
    }, drop));
    EXPECT_EQ(2u, revision);
    EXPECT_TRUE(state.FinishEmpty(state.Published(), drop));
    EXPECT_EQ(1u, lost);
    EXPECT_EQ(2u, state.Completed());
    EXPECT_EQ(1u, events.Published());
    EXPECT_EQ(0u, events.Completed());
}
} // namespace
