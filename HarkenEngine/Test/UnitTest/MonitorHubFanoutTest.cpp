#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "Common/TestUtil.h"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

VnMonitorSessionDesc MakeTimeSeriesDesc(std::uint64_t queueBytes) {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_MEASUREMENTS;
    desc.consumerQueueBytes = queueBytes;
    return desc;
}

struct PollArgs {
    std::vector<unsigned char> buffer;
    VnMonitorPollDesc desc{};
    VnMonitorPollResult result{};

    explicit PollArgs(std::uint64_t bytes = 4096) : buffer(static_cast<std::size_t>(bytes)) {
        desc.structSize = sizeof(desc);
        desc.buffer = buffer.data();
        desc.bufferBytes = buffer.size();
        result.structSize = sizeof(result);
    }
};

void WaitDrainEntered(std::uint32_t count) {
    const auto start = std::chrono::steady_clock::now();
    while (harken::monitor::DrainEnteredForTests() < count &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::yield();
    }
}

void WaitFanoutEntered(std::uint32_t count) {
    const auto start = std::chrono::steady_clock::now();
    while (harken::monitor::FanoutEnteredForTests() < count &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::yield();
    }
}

std::uint64_t HubFanoutDropCount(VnMonitorSessionHandle handle) {
    VnMonitorDropSummary summaries[4]{};
    uint32_t required = 0;
    if (Vn_MonitorGetConsumerStatus(handle, summaries, 4, &required) != VN_OK) {
        return 0;
    }
    for (uint32_t i = 0; i < required; ++i) {
        if (summaries[i].stage == VN_MONITOR_DROP_HUB_FANOUT) {
            return summaries[i].recordCount;
        }
    }
    return 0;
}

struct FanoutHoldGuard {
    FanoutHoldGuard() { harken::monitor::SetFanoutHoldForTests(true); }
    ~FanoutHoldGuard() { harken::monitor::SetFanoutHoldForTests(false); }
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

struct LiveHeader {
    std::uint32_t recordKind = 0;
    std::uint64_t sourceSequence = 0;
    std::uint64_t deliverySequence = 0;
};

LiveHeader ReadLiveHeader(const PollArgs& poll) {
    LiveHeader header{};
    EXPECT_GE(poll.result.bytesWritten, 300u);
    if (poll.result.bytesWritten < 300u) {
        return header;
    }
    header.recordKind = ReadU32(poll.buffer.data());
    header.sourceSequence = ReadU64(poll.buffer.data() + 40);
    header.deliverySequence = ReadU64(poll.buffer.data() + 48);
    return header;
}

} // namespace

TEST(MonitorHubFanout, TwoSessionsIndependentlyReceiveSameSource) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());

    VnMonitorSessionDesc desc = MakeTimeSeriesDesc(4096);
    VnMonitorSessionHandle a = 0;
    VnMonitorSessionHandle b = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &a));
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &b));

    harken::monitor::StopDrainForTests();
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs pollA;
    PollArgs pollB;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(a, &pollA.desc, &pollA.result));
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(b, &pollB.desc, &pollB.result));
    EXPECT_EQ(1u, pollA.result.recordsWritten);
    EXPECT_EQ(1u, pollB.result.recordsWritten);
    EXPECT_EQ(1u, pollA.result.firstDeliverySequence);
    EXPECT_EQ(1u, pollB.result.firstDeliverySequence);

    const LiveHeader recA = ReadLiveHeader(pollA);
    const LiveHeader recB = ReadLiveHeader(pollB);
    EXPECT_EQ(recA.sourceSequence, recB.sourceSequence);
    EXPECT_NE(0u, recA.sourceSequence);
    EXPECT_EQ(1u, recA.deliverySequence);
    EXPECT_EQ(1u, recB.deliverySequence);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_RECORD_TIME_SERIES),
              recA.recordKind);

    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(a));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(b));
}

TEST(MonitorHubFanout, BufferTooSmallDoesNotConsume) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc = MakeTimeSeriesDesc(4096);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    harken::monitor::StopDrainForTests();
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs empty(0);
    empty.desc.buffer = nullptr;
    empty.desc.bufferBytes = 0;
    EXPECT_EQ(VN_MONITOR_BUFFER_TOO_SMALL,
              Vn_MonitorPoll(handle, &empty.desc, &empty.result));
    EXPECT_GT(empty.result.nextRecordBytes, 0u);
    EXPECT_EQ(0u, empty.result.recordsWritten);

    PollArgs tiny(8);
    EXPECT_EQ(VN_MONITOR_BUFFER_TOO_SMALL,
              Vn_MonitorPoll(handle, &tiny.desc, &tiny.result));
    EXPECT_GT(tiny.result.nextRecordBytes, 8u);

    PollArgs ok;
    EXPECT_EQ(VN_OK, Vn_MonitorPoll(handle, &ok.desc, &ok.result));
    EXPECT_EQ(1u, ok.result.recordsWritten);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubFanout, DrainWakesAfterIdleAndRestart) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc = MakeTimeSeriesDesc(8192);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));

    std::uint64_t expectedSequence = 1;
    for (std::uint32_t cycle = 0; cycle < 16; ++cycle) {
        SCOPED_TRACE(cycle);
        const bool restart = cycle % 2 == 0;
        if (restart) {
            harken::monitor::StopDrainForTests();
        }
        const std::uint32_t count = restart ? 8 : 1;
        ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(count));
        if (restart) {
            harken::monitor::StartDrainForTests();
        }
        PollArgs poll(8192);
        std::uint32_t received = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (received < count && std::chrono::steady_clock::now() < deadline) {
            ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll.desc, &poll.result));
            if (poll.result.recordsWritten != 0) {
                EXPECT_EQ(expectedSequence, poll.result.firstDeliverySequence);
                received += poll.result.recordsWritten;
                expectedSequence += poll.result.recordsWritten;
                EXPECT_EQ(expectedSequence - 1, poll.result.lastDeliverySequence);
            } else {
                std::this_thread::yield();
            }
        }
        ASSERT_EQ(count, received);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubFanout, SlowSessionDropsFanoutOnly) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::uint32_t packed = harken::monitor::PackedTimeSeriesBytesForTests();
    const std::uint64_t tiny = packed + 64u;
    VnMonitorSessionHandle slow = 0;
    VnMonitorSessionHandle fast = 0;
    VnMonitorSessionDesc slowDesc = MakeTimeSeriesDesc(tiny);
    VnMonitorSessionDesc fastDesc = MakeTimeSeriesDesc(4096);
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&slowDesc, &slow));
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&fastDesc, &fast));

    harken::monitor::StopDrainForTests();
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(3));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs fastPoll(8192);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(fast, &fastPoll.desc, &fastPoll.result));
    EXPECT_EQ(3u, fastPoll.result.recordsWritten);
    EXPECT_EQ(1u, fastPoll.result.firstDeliverySequence);
    EXPECT_EQ(3u, fastPoll.result.lastDeliverySequence);
    std::uint64_t offset = 0;
    std::uint64_t firstSource = 0;
    for (std::uint64_t i = 0; i < 3; ++i) {
        ASSERT_LE(offset + 48, fastPoll.result.bytesWritten);
        const std::uint64_t source =
            ReadU64(fastPoll.buffer.data() + offset + 40);
        if (i == 0) {
            firstSource = source;
            EXPECT_NE(0u, firstSource);
        } else {
            EXPECT_EQ(firstSource + i, source);
        }
        offset += packed;
    }

    PollArgs slowPoll(8192);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(slow, &slowPoll.desc, &slowPoll.result));
    EXPECT_LE(slowPoll.result.recordsWritten, 1u);
    const std::uint32_t kept = slowPoll.result.recordsWritten;
    if (kept == 1u) {
        EXPECT_EQ(firstSource, ReadU64(slowPoll.buffer.data() + 40));
    }

    VnMonitorDropSummary summaries[4]{};
    uint32_t required = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorGetConsumerStatus(slow, summaries, 4, &required));
    ASSERT_GE(required, 1u);
    bool sawFanout = false;
    for (uint32_t i = 0; i < required; ++i) {
        if (summaries[i].stage == VN_MONITOR_DROP_HUB_FANOUT) {
            sawFanout = true;
            EXPECT_EQ(1u, summaries[i].streamId);
            EXPECT_EQ(3u - kept, summaries[i].recordCount);
            EXPECT_EQ(firstSource + kept, summaries[i].firstSourceSequence);
            EXPECT_EQ(firstSource + 2u, summaries[i].lastSourceSequence);
            EXPECT_EQ(static_cast<std::uint64_t>(packed) *
                          (3u - kept),
                      summaries[i].byteCount);
        }
    }
    EXPECT_TRUE(sawFanout);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(slow));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(fast));
}

TEST(MonitorHubFanout, InternalRecorderAndRemoteAreIndependent) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::EnableInternalConsumersForTests(7);
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(2));
    harken::monitor::DrainOnceForTests();
    EXPECT_GE(harken::monitor::InternalQueuedCountForTests(0), 1u);
    EXPECT_GE(harken::monitor::InternalQueuedCountForTests(1), 1u);
    EXPECT_GE(harken::monitor::InternalQueuedCountForTests(2), 1u);
    const VnMonitorDropSummary recorder = harken::monitor::InternalDropForTests(0);
    EXPECT_TRUE(recorder.stage == 0 ||
                recorder.stage == VN_MONITOR_DROP_RECORDER_QUEUE);
}

TEST(MonitorHubFanout, ProducerRingDropUsesSourceStage) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::StopDrainForTests();
    const std::uint32_t capacity = harken::monitor::ProducerRingCapacityForTests();
    ASSERT_GT(capacity, 0u);
    const bool fillOk =
        harken::monitor::PublishSyntheticTimeSeriesForTests(capacity);
    EXPECT_TRUE(fillOk);
    const bool overflow =
        harken::monitor::PublishSyntheticTimeSeriesForTests(1);
    EXPECT_FALSE(overflow);
    const VnMonitorDropSummary producer = harken::monitor::ProducerDropForTests();
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_DROP_SOURCE_PRODUCER),
              producer.stage);
    EXPECT_EQ(1u, producer.streamId);
    EXPECT_EQ(1u, producer.recordCount);
    EXPECT_EQ(static_cast<std::uint64_t>(capacity) + 1u,
              producer.firstSourceSequence);
    EXPECT_EQ(producer.firstSourceSequence, producer.lastSourceSequence);
    EXPECT_EQ(harken::monitor::PackedTimeSeriesBytesForTests(), producer.byteCount);
    harken::monitor::StartDrainForTests();

    VnMonitorSessionDesc desc = MakeTimeSeriesDesc(4096);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    uint32_t required = 0;
    EXPECT_EQ(VN_MONITOR_BUFFER_TOO_SMALL,
              Vn_MonitorGetConsumerStatus(handle, nullptr, 0, &required));
    EXPECT_EQ(1u, required);
    VnMonitorDropSummary one{};
    required = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorGetConsumerStatus(handle, &one, 1, &required));
    EXPECT_EQ(1u, required);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_DROP_SOURCE_PRODUCER),
              one.stage);
    EXPECT_GE(one.recordCount, 1u);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubFanout, CloseThenOpenSameSlotDoesNotReceiveStaleFanout) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc = MakeTimeSeriesDesc(4096);
    VnMonitorSessionHandle first = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &first));

    harken::monitor::StopDrainForTests();
    harken::monitor::ResetFanoutEnteredForTests();
    FanoutHoldGuard hold;
    harken::monitor::StartDrainForTests();
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    WaitFanoutEntered(1);
    ASSERT_GE(harken::monitor::FanoutEnteredForTests(), 1u);

    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(first));
    VnMonitorSessionHandle second = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &second));
    EXPECT_NE(first, second);

    harken::monitor::SetFanoutHoldForTests(false);
    const auto waitStart = std::chrono::steady_clock::now();
    while (harken::monitor::FanoutEnteredForTests() < 2u &&
           std::chrono::steady_clock::now() - waitStart <
               std::chrono::milliseconds(200)) {
        std::this_thread::yield();
    }

    PollArgs stalePoll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(second, &stalePoll.desc, &stalePoll.result));
    EXPECT_EQ(0u, stalePoll.result.recordsWritten);

    harken::monitor::StopDrainForTests();
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs freshPoll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(second, &freshPoll.desc, &freshPoll.result));
    EXPECT_EQ(1u, freshPoll.result.recordsWritten);
    EXPECT_GT(ReadLiveHeader(freshPoll).sourceSequence, 0u);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(second));
}

TEST(MonitorHubFanout, CloseWhileDrainDoesNotUaf) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc = MakeTimeSeriesDesc(4096);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    harken::monitor::ResetDrainEnteredForTests();
    harken::monitor::SetDrainHoldForTests(true);
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    WaitDrainEntered(1);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    harken::monitor::SetDrainHoldForTests(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubFanout, TermWhileDrainDoesNotUaf) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc = MakeTimeSeriesDesc(4096);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    harken::monitor::ResetDrainEnteredForTests();
    harken::monitor::SetDrainHoldForTests(true);
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    WaitDrainEntered(1);
    ASSERT_EQ(VN_OK, Vn_Term());
    engine.Release();
    harken::monitor::SetDrainHoldForTests(false);
    harkentest::EngineSession retry(harkentest::MakeNullConfig());
    EXPECT_EQ(VN_OK, retry.InitResult());
}

TEST(MonitorHubFanout, InitFailAfterThreadLeavesNoOrphan) {
    harken::monitor::SetFailInitAfterThreadForTests(true);
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    EXPECT_EQ(VN_IN_USE, engine.InitResult());
    EXPECT_FALSE(harken::monitor::DrainRunningForTests());
    EXPECT_FALSE(harken::monitor::CaptureResourcesAllocatedForTests());
    harken::monitor::SetFailInitAfterThreadForTests(false);
    harkentest::EngineSession retry(harkentest::MakeNullConfig());
    EXPECT_EQ(VN_OK, retry.InitResult());
    EXPECT_FALSE(harken::monitor::DrainRunningForTests());
    EXPECT_FALSE(harken::monitor::CaptureResourcesAllocatedForTests());
}

TEST(MonitorHubFanout, PublishPathNoMutexInAudioScope) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::StartDrainForTests();
    const std::uint64_t before = harken::memguard::HitCount();
    {
        harken::memguard::AudioThreadScope scope;
        ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    }
    EXPECT_EQ(before, harken::memguard::HitCount());
    EXPECT_EQ(0u, harken::monitor::PublishMutexHitsForTests());
}

TEST(MonitorHubFanout, CaptureOffPassthroughUnchanged) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> sine =
        harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc desc = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    ASSERT_EQ(sine.size(), out.left.size());
    ASSERT_EQ(sine.size(), out.right.size());
    for (std::size_t i = 0; i < sine.size(); ++i) {
        const float expected = sine[i];
        ASSERT_EQ(expected, out.left[i]) << "left sample " << i;
        ASSERT_EQ(expected, out.right[i]) << "right sample " << i;
    }
}

TEST(MonitorHubFanout, SlowConsumerPumpStaysBitExact) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionDesc desc = MakeTimeSeriesDesc(64);
    VnMonitorSessionHandle slow = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &slow));
    const std::vector<float> sine =
        harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(2));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const std::uint64_t dropBefore = HubFanoutDropCount(slow);
    harken::monitor::StopDrainForTests();
    const bool seeded = harken::monitor::PublishSyntheticTimeSeriesForTests(8);
    harken::monitor::DrainOnceForTests();
    const std::uint64_t dropAfter = HubFanoutDropCount(slow);
    harken::monitor::StartDrainForTests();
    ASSERT_TRUE(seeded);
    ASSERT_GT(dropAfter, dropBefore)
        << "slow consumer did not publish a fresh HUB_FANOUT drop";

    // Manual NullBackend pumping proves functional isolation only. Real device
    // callback timing belongs to the Runtime device qualification matrix.
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    ASSERT_EQ(sine.size(), out.left.size());
    ASSERT_EQ(sine.size(), out.right.size());
    for (std::size_t i = 0; i < sine.size(); ++i) {
        const float expected = sine[i];
        ASSERT_EQ(expected, out.left[i]) << "left sample " << i;
        ASSERT_EQ(expected, out.right[i]) << "right sample " << i;
    }
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(slow));
}
