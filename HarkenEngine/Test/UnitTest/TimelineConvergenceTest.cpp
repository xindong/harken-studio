#include "Common/ObjectIdTestSupport.h"
#include "DeviceBackend.h"
#include "Engine.h"
#include "Common/TestUtil.h"
#include "harken/core/AllocGuard.h"
#include "harken/core/TimelineRuntimeInstall.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace {

class DigestSink final : public harken::TimelineDigestSink {
public:
    void Fold(const harken::TimelineDigestRecord& record) noexcept override {
        ++count;
        hash ^= record.disposition;
        hash *= 1099511628211ull;
        hash ^= record.event.eventIndex;
        hash *= 1099511628211ull;
        hash ^= record.effectiveFrame;
        hash *= 1099511628211ull;
    }

    std::uint32_t count = 0;
    std::uint64_t hash = 14695981039346656037ull;
};

enum class TimelineDriverMode : std::uint8_t {
    Simulator = 0,
    Offline = 1,
    Realtime = 2,
};

struct DriverResult {
    std::uint64_t digest = 0;
    harken::TimelineExecutor::Counters counters{};
    std::vector<float> samples;
    std::uint32_t digestCount = 0;
    std::uint64_t sinkDigest = 0;
    bool memguardArmed = false;
    std::uint64_t allocationHitsBefore = 0;
    std::uint64_t allocationHitsAfter = 0;
    std::vector<std::uint32_t> armTrace;
    std::vector<std::uint64_t> pageTrace;
};

DriverResult RunTimeline(TimelineDriverMode mode) {
    const bool realtime = mode == TimelineDriverMode::Realtime;
    harken::DeviceBackendTestState deviceState{};
    struct DeviceStateGuard {
        explicit DeviceStateGuard(harken::DeviceBackendTestState* state)
            : active(state != nullptr) {
            if (active) {
                harken::SetDeviceBackendTestState(state);
            }
        }
        ~DeviceStateGuard() {
            if (active) {
                harken::SetDeviceBackendTestState(nullptr);
            }
        }
        bool active = false;
    } deviceGuard(realtime ? &deviceState : nullptr);
    (void)deviceGuard;

    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    if (realtime) {
        config.deviceMode = VN_DEVICE_REAL;
        config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    }
    harkentest::EngineSession session(config);
    EXPECT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    EXPECT_NE(engine, nullptr);

    const std::vector<float> pcm(1024, 0.25f);
    const VnPCMDesc pcmDesc = harkentest::MonoDesc(pcm, 48000);
    EXPECT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &pcmDesc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 71;
    target.clipGuidHigh = 11;
    target.subjectGuidHigh = 13;
    target.trackId = harkentest::ObjectId(17);
    target.clipId = harkentest::ObjectId(19);
    target.subjectId = harkentest::ObjectId(23);
    target.occurrenceOrdinal = 1;

    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;

    std::array<harken::EventTemplate, 2> events{};
    events[0] = {256, 1, 0, 0, 20, 0};
    events[1] = {768, 1, 1, 0, 50, 1};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;

    harken::TimelineProgramView program{1, payloads.data(),
                                      static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    EXPECT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);

    const auto ledgerRows = engine->ledger.ConstructedEntryCount();
    const auto ledgerBytes = engine->ledger.EntryStorageBytes();
    const auto receiptBytes = engine->ledger.MonitorReceiptStorageBytes();
    const auto ledgerPages = engine->ledger.EntryCommittedBytes();
    const auto receiptPages = engine->ledger.MonitorReceiptCommittedBytes();
    engine->ledger.SetEntryCommitPageLimitForTests(0);
    engine->ledger.SetReceiptCommitPageLimitForTests(0);

    DigestSink digestSink;
    engine->timelineExecutor.SetDigestSink(&digestSink);
    EXPECT_EQ(Vn_Update(), VN_OK);

    DriverResult result;
    auto recordTrace = [&]() {
        harken::ArmState state = harken::ArmState::Free;
        if (installed.generation != 0 &&
            engine->timelineProgramSets[installed.generation & 1u] != nullptr) {
            const harken::EngineTimelineOccurrence& live =
                engine->timelineProgramSets[installed.generation & 1u]
                    ->occurrences[0];
            (void)engine->timelineArms.State(live.arm, &state);
        }
        const std::uint32_t value = static_cast<std::uint32_t>(state);
        if (result.armTrace.empty() || result.armTrace.back() != value) {
            result.armTrace.push_back(value);
        }
        result.pageTrace.push_back(
            engine->timelinePool.ConsumerFrontier().load());
        result.pageTrace.push_back(
            engine->timelinePool.PublishCutoffFrame());
    };
    recordTrace();
    result.memguardArmed = harken::memguard::IsArmed();
    result.allocationHitsBefore = harken::memguard::HitCount();
    constexpr std::uint32_t kMaxBlocks = 12;
    constexpr std::uint32_t kChannels = 2;
    if (realtime) {
        std::array<float, 256u * kChannels> blockOutput{};
        for (std::uint32_t block = 0; block < kMaxBlocks; ++block) {
            EXPECT_TRUE(engine->device.PumpCallbackForTests(
                blockOutput.data(), 256));
            result.samples.insert(result.samples.end(), blockOutput.begin(),
                                  blockOutput.end());
            EXPECT_EQ(Vn_Update(), VN_OK);
            recordTrace();
            if (digestSink.count != 0 && block >= 3) {
                break;
            }
        }
    } else {
        for (std::uint32_t block = 0; block < kMaxBlocks; ++block) {
            if (mode == TimelineDriverMode::Simulator) {
                engine->timelineService->Service();
            }
            engine->nullBackend.Pump(1);
            const float* output = engine->nullBackend.LastBlock();
            result.samples.insert(result.samples.end(), output,
                                  output + 256u * kChannels);
            if (mode == TimelineDriverMode::Simulator) {
                engine->timelineService->Service();
            } else {
                EXPECT_EQ(Vn_Update(), VN_OK);
            }
            recordTrace();
            if (digestSink.count != 0 && block >= 3) {
                break;
            }
        }
    }
    result.counters = engine->timelineExecutor.Stats();
    result.digest = result.counters.digest;
    result.digestCount = digestSink.count;
    result.sinkDigest = digestSink.hash;
    result.allocationHitsAfter = harken::memguard::HitCount();

    EXPECT_EQ(ledgerRows, engine->ledger.ConstructedEntryCount());
    EXPECT_EQ(ledgerBytes, engine->ledger.EntryStorageBytes());
    EXPECT_EQ(receiptBytes, engine->ledger.MonitorReceiptStorageBytes());
    EXPECT_EQ(ledgerPages, engine->ledger.EntryCommittedBytes());
    EXPECT_EQ(receiptPages, engine->ledger.MonitorReceiptCommittedBytes());

    return result;
}

TEST(TimelineConvergence, FixedProductionSequenceRendersTimelineEventsAndAudio) {
    harken::DeviceBackendTestState deviceState{};
    struct DeviceStateGuard {
        explicit DeviceStateGuard(harken::DeviceBackendTestState& state) {
            harken::SetDeviceBackendTestState(&state);
        }
        ~DeviceStateGuard() {
            harken::SetDeviceBackendTestState(nullptr);
        }
    } guard(deviceState);

    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    config.deviceMode = VN_DEVICE_REAL;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> pcm(1024, 0.25f);
    const VnPCMDesc pcmDesc = harkentest::MonoDesc(pcm, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &pcmDesc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 72;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    ASSERT_EQ(harken::InstallTimelineProgram(install).status,
              harken::TimelineRuntimeInstallStatus::Installed);

    DigestSink digestSink;
    engine->timelineExecutor.SetDigestSink(&digestSink);
    ASSERT_EQ(Vn_Update(), VN_OK);
    std::array<float, 256u * 2u> q0{};
    std::array<float, 256u * 2u> q1{};
    ASSERT_TRUE(engine->device.PumpCallbackForTests(q0.data(), 256));
    ASSERT_EQ(Vn_Update(), VN_OK);
    ASSERT_TRUE(engine->device.PumpCallbackForTests(q1.data(), 256));

    EXPECT_GT(digestSink.count, 0u);
    EXPECT_TRUE(std::any_of(q1.begin(), q1.end(),
                            [](float sample) { return sample != 0.0f; }));
    EXPECT_EQ(engine->timelineExecutor.Stats().deadlineMissQuanta, 0u);
    EXPECT_EQ(engine->timelineExecutor.Stats().renderUnavailableDeferrals, 0u);
    EXPECT_EQ(engine->timelineExecutor.Stats().protocolViolation, 0u);
}

TEST(TimelineConvergence, ProductionTimelinePathAllocatesNothingOnAudioThread) {
    const DriverResult offline = RunTimeline(TimelineDriverMode::Offline);
    EXPECT_GT(offline.digestCount, 0u);
#ifdef _DEBUG
    EXPECT_TRUE(offline.memguardArmed);
#endif
    EXPECT_EQ(offline.allocationHitsBefore, offline.allocationHitsAfter);
}

TEST(TimelineConvergence, SimulatorOfflineAndRealtimeShareExecutionFacts) {
    const DriverResult simulator = RunTimeline(TimelineDriverMode::Simulator);
    const DriverResult offline = RunTimeline(TimelineDriverMode::Offline);
    const DriverResult realtime = RunTimeline(TimelineDriverMode::Realtime);

    EXPECT_GT(simulator.digestCount, 0u);
    EXPECT_TRUE(std::any_of(simulator.samples.begin(), simulator.samples.end(),
                            [](float sample) { return sample != 0.0f; }));
    EXPECT_EQ(simulator.digest, offline.digest);
    EXPECT_EQ(simulator.digestCount, offline.digestCount);
    EXPECT_EQ(simulator.sinkDigest, offline.sinkDigest);
    EXPECT_EQ(simulator.samples, offline.samples);
    EXPECT_EQ(simulator.armTrace, offline.armTrace);
    EXPECT_EQ(simulator.pageTrace, offline.pageTrace);
    EXPECT_FALSE(simulator.armTrace.empty());
    EXPECT_FALSE(simulator.pageTrace.empty());
    EXPECT_EQ(offline.digest, realtime.digest);
    EXPECT_EQ(offline.digestCount, realtime.digestCount);
    EXPECT_EQ(offline.sinkDigest, realtime.sinkDigest);
    EXPECT_EQ(offline.samples, realtime.samples);
    EXPECT_EQ(offline.armTrace, realtime.armTrace);
    EXPECT_EQ(offline.pageTrace, realtime.pageTrace);
    EXPECT_EQ(simulator.counters.deadlineMissQuanta, 0u);
    EXPECT_EQ(simulator.counters.renderUnavailableDeferrals, 0u);
    EXPECT_EQ(simulator.counters.protocolViolation, 0u);
    EXPECT_EQ(simulator.counters.armFailedTransactions, 0u);
    EXPECT_EQ(offline.counters.deadlineMissQuanta, 0u);
    EXPECT_EQ(realtime.counters.deadlineMissQuanta, 0u);
    EXPECT_EQ(offline.counters.renderUnavailableDeferrals, 0u);
    EXPECT_EQ(realtime.counters.renderUnavailableDeferrals, 0u);
    EXPECT_EQ(offline.counters.protocolViolation, 0u);
    EXPECT_EQ(realtime.counters.protocolViolation, 0u);
    EXPECT_EQ(offline.counters.armFailedTransactions, 0u);
    EXPECT_EQ(realtime.counters.armFailedTransactions, 0u);
    EXPECT_GT(offline.digestCount, 0u);
}

} // namespace
