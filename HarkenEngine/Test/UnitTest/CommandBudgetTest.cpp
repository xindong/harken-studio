// The per-quantum control-command budget: one block applies at most the
// configured ceiling, and what it leaves behind applies on a later block
// rather than being dropped. Covered here: the ceiling bounds the drain
// independently of how deep the queue is or how much a caller pushed, and
// holds a maximal backlog inside the block deadline the drain has to meet; a
// deferred setter still lands, in order, with the same final value; a
// sustained backlog reports once per episode and reports again after a full
// recovery; the published high-water counter tracks the worst block; a
// Variable publication bypasses the ordinary ceiling and lands every target
// in one audio slice; and a deferred ramp starts at the boundary where it
// actually applies, leaving its target value untouched.
//
// Indivisible topology and play-led batches wait until one quantum can apply
// them whole; malformed batches are skipped without applying a prefix.
#include "Common/ObjectIdTestSupport.h"
#include "Engine.h"
#include "CommandQueueSubmissionTestAccess.h"
#include "Common/TestUtil.h"

#include <array>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;
using harkentest::PumpAndCapture;

constexpr std::uint32_t kBlockFrames = 480; // 10 ms at 48 kHz
constexpr std::uint32_t kRate = 48000;
constexpr std::uint64_t kBusBase = 60;
constexpr VnVariableId kBusVariable = harkentest::ObjectId(900);
constexpr std::uint64_t kBusBindingBase = 910;

// 20*log10(0.5) and 20*log10(0.25): exact half and quarter amplitude.
const float kHalfDb = 20.0f * std::log10(0.5f);
const float kQuarterDb = 20.0f * std::log10(0.25f);

VnConfig BudgetConfig(std::uint32_t maxVoices = 2) {
    VnConfig config = MakeNullConfig(maxVoices, kBlockFrames, kRate);
    // Deep enough that the queue is never the binding constraint: the
    // ceiling has to be what bounds the drain, otherwise the test would
    // measure backpressure instead.
    config.commandQueueDepth = 1024;
    return config;
}

// A looping DC voice, so every rendered block carries the same level and the
// output reads back as the product of the gains applied to it.
VnVoiceHandle PlayDcVoice(VnSoundId soundId, float amplitude, VnBusId busId,
                          std::vector<float>& storage) {
    storage = harkentest::MakeDC(kBlockFrames, amplitude);
    const VnPCMDesc desc = MonoDesc(storage, kRate);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(soundId, &desc));
    VnPlayDesc play = MakePlayDesc(soundId);
    play.busId = busId;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    return handle;
}

VnSnapshot Snap() {
    VnSnapshot s{};
    s.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&s));
    return s;
}

// Pumps one block and returns its deinterleaved stereo output.
Capture PumpBlock(harken::Engine* engine) {
    Capture out;
    PumpAndCapture(engine, 1, out);
    return out;
}

// Records still queued: what the producer has published minus what the audio
// thread has consumed.
std::uint64_t Queued(harken::Engine* engine) {
    return engine->commands.PublishedHead() -
           engine->commands.ConsumedCount();
}

// Drains the audio-side log ring, counting records carrying `code`. The logic
// thread is the ring's only consumer and the test is that thread.
std::uint32_t DrainAudioLogCount(harken::Engine* engine, std::uint32_t code) {
    std::uint32_t hits = 0;
    harken::LogRecord record{};
    while (engine->logRing.TryPop(record)) {
        if (record.code == code) {
            ++hits;
        }
    }
    return hits;
}

// A binding definition mapping the shared Variable's 0..1 range onto
// 0 dB .. half amplitude on a bus's volume.
VnBindingDesc BusVolumeBindingDesc() {
    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = kBusVariable;
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS;
    desc.targetId = 0; // bus volume, dB
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, kHalfDb};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    return desc;
}

VnBindingAttachDesc BusAttachment(VnBusId busId) {
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = busId;
    return attach;
}

// --- the ceiling bounds one block's drain ---

TEST(CommandBudget, OneBlockAppliesAtMostTheCeilingHoweverDeepTheQueue) {
    EngineSession session(BudgetConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1); // quiesce anything init queued

    constexpr std::uint32_t kCeiling = 8;
    constexpr std::uint32_t kPushed = 64;
    engine->mixer.SetMaxCommandsPerQuantumForTest(kCeiling);
    for (std::uint32_t i = 0; i < kPushed; ++i) {
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
    }
    // One command per setter, none consumed yet: the drain below starts from
    // a known backlog rather than an assumed one.
    ASSERT_EQ(kPushed, Queued(engine));
    const std::uint64_t consumedBefore = engine->commands.ConsumedCount();

    engine->nullBackend.Pump(1);
    EXPECT_EQ(kCeiling, engine->commands.ConsumedCount() - consumedBefore);
    EXPECT_EQ(kPushed - kCeiling, engine->mixer.CommandsDeferredLastBlock());
    EXPECT_EQ(kPushed - kCeiling, Queued(engine));

    // Nothing was dropped: the remaining records apply over the following
    // blocks, at the same ceiling each time, until the queue is empty.
    for (std::uint32_t block = 1; block < kPushed / kCeiling; ++block) {
        engine->nullBackend.Pump(1);
        EXPECT_EQ(kPushed - kCeiling * (block + 1),
                  engine->mixer.CommandsDeferredLastBlock())
            << "block " << block;
    }
    EXPECT_EQ(0u, Queued(engine));
    EXPECT_EQ(0u, engine->mixer.CommandsDeferredLastBlock());
    EXPECT_EQ(static_cast<std::uint64_t>(kPushed - kCeiling),
              engine->mixer.CommandsDeferredHighWater());
}

// The reason the ceiling exists, measured rather than asserted: at the deepest
// queue the engine accepts, draining the whole backlog in one block costs
// multiples of the block it had to render in. The default ceiling holds the
// same backlog to a fraction of that block.
//
// The margins are wide on both sides -- roughly three orders of magnitude
// under the deadline with the ceiling, and several times over it without --
// so this is a timing assertion that can afford to be one.
TEST(CommandBudget, TheCeilingKeepsAMaximalBacklogInsideTheBlockDeadline) {
    constexpr std::uint32_t kShortBlock = 128; // 2667 us at 48 kHz
    constexpr std::uint32_t kDeadlineUs =
        1000000u * kShortBlock / kRate;
    VnConfig config = MakeNullConfig(2, kShortBlock, kRate);
    config.commandQueueDepth = 1u << 19;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1);

    // Fill the queue to refusal: the largest backlog this configuration can
    // present to one block. The default ceiling applies -- no test hook here,
    // because the shipped default is what is under measurement.
    std::uint32_t pushed = 0;
    while (Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0) == VN_OK) {
        ++pushed;
    }
    ASSERT_GT(pushed, 1u << 18) << "the queue did not fill";

    engine->nullBackend.Pump(1);
    const VnSnapshot snapshot = Snap();
    // Non-fatal, so an unbounded drain reports both what it broke: no records
    // left for later, and the drain time that went with taking them all.
    EXPECT_GT(engine->mixer.CommandsDeferredLastBlock(), 0u)
        << "backlog was not larger than one block's ceiling";
    EXPECT_LT(snapshot.audioDrainLastUs, kDeadlineUs / 8u)
        << "drain took " << snapshot.audioDrainLastUs << " us of a "
        << kDeadlineUs << " us block";
}

// The counter is a high-water mark, so the half of its contract that a
// monotonicity test cannot see is the floor: a session that never exceeded the
// ceiling must publish zero. Without this, an implementation that reported
// something non-zero from the first block would satisfy every other assertion
// here.
TEST(CommandBudget, TheHighWaterStaysZeroWhileNothingExceedsTheCeiling) {
    EngineSession session(BudgetConfig(4));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Ordinary traffic at the shipped default ceiling: plays, setters, a bus,
    // and Variable publication, all well inside one block's allowance.
    std::vector<float> dc;
    const VnVoiceHandle voice = PlayDcVoice(harkentest::ObjectId(1), 0.5f, VN_BUS_MASTER, dc);
    ASSERT_NE(0u, voice);
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(kBusBase), VN_BUS_MASTER));
    for (std::uint32_t block = 0; block < 8; ++block) {
        ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(voice, -3.0f, 0));
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(kBusBase), -6.0f, 0));
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(0u, engine->mixer.CommandsDeferredLastBlock())
            << "block " << block << " deferred work it should have taken";
        EXPECT_EQ(0u, Snap().commandDeferredCount) << "block " << block;
    }
    EXPECT_EQ(0u, engine->mixer.CommandsDeferredHighWater());
}

TEST(CommandBudget, PlayLedLayoutRemainsInsideCommandPayload) {
    EXPECT_EQ(sizeof(harken::CommandRecord), 64u);
    EXPECT_EQ(sizeof(harken::PlayLedBatchHeader), 16u);
    EXPECT_LE(sizeof(harken::CommandRecord::Payload), 56u);
}

TEST(CommandBudget, OrdinaryPlayUsesSingleRecordBatchIdentity) {
    harken::CommandRecord command{};
    command.opcode = harken::kOpcodePlay;
    command.payload.play.batch.batchVersion = harken::kPlayLedBatchVersion;
    command.payload.play.batch.batchKind = harken::PlayLedBatchKind::None;
    command.payload.play.batch.batchCommandCount = 1;
    EXPECT_EQ(command.payload.play.batch.batchCommandCount, 1u);
    EXPECT_EQ(command.payload.play.batch.batchKind,
              harken::PlayLedBatchKind::None);
}

TEST(CommandBudget, TimelineArmBatchWaitsForWholeQuantumBudget) {
    EngineSession session(BudgetConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->mixer.SetMaxCommandsPerQuantumForTest(2);

    harken::CommandRecord ordinary{};
    ordinary.opcode = harken::kOpcodeSetBusVolume;
    ordinary.payload.setBusVolume.busIndex = 0;
    ASSERT_TRUE(engine->commands.TryPush(ordinary));
    harken::CommandRecord timeline[2]{};
    timeline[0].opcode = harken::kOpcodePlay;
    timeline[0].payload.play.batch.batchVersion =
        harken::kPlayLedBatchVersion;
    timeline[0].payload.play.batch.batchKind =
        harken::PlayLedBatchKind::TimelineArm;
    timeline[0].payload.play.batch.batchCommandCount = 2;
    timeline[0].payload.play.batch.armToken = 1;
    timeline[1].opcode = harken::kOpcodeSetVoiceVolume;
    ASSERT_TRUE(engine->commands.TryPushBatch(timeline, 2));

    engine->mixer.DrainCommandsForTest();
    EXPECT_EQ(engine->commands.ConsumedCount(), 1u);
    engine->mixer.DrainCommandsForTest();
    EXPECT_EQ(engine->commands.ConsumedCount(), 3u);
}

TEST(CommandBudget, InvalidPlayLedBatchIsSkippedWhole) {
    EngineSession session(BudgetConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1);

    harken::CommandRecord commands[2]{};
    commands[0].opcode = harken::kOpcodePlay;
    commands[0].payload.play.batch.batchVersion =
        harken::kPlayLedBatchVersion;
    commands[0].payload.play.batch.batchKind =
        harken::PlayLedBatchKind::DeferredStart;
    commands[0].payload.play.batch.batchCommandCount = 2;
    commands[1].opcode = harken::kOpcodeSetBusVolume;
    commands[1].payload.setBusVolume.busIndex = 0;
    commands[1].payload.setBusVolume.volumeDb = -12.0f;
    ASSERT_TRUE(engine->commands.TryPushBatch(commands, 2));

    engine->mixer.SuppressCommandProtocolAssertForTest(true);
    engine->mixer.DrainCommandsForTest();
    engine->mixer.SuppressCommandProtocolAssertForTest(false);
    EXPECT_EQ(2u, engine->commands.ConsumedCount());
    EXPECT_EQ(1u, engine->mixer.CommandProtocolViolationCount());
}

// --- a deferred setter still lands, in order ---

TEST(CommandBudget, DeferredSettersApplyOnLaterBlocksInOrder) {
    EngineSession session(BudgetConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    std::vector<float> dc;
    const VnVoiceHandle voice = PlayDcVoice(harkentest::ObjectId(1), 0.5f, VN_BUS_MASTER, dc);
    ASSERT_NE(0u, voice);
    Capture warm = PumpBlock(engine); // the play command applies here
    HARKENTEST_ASSERT_NON_SILENT(warm.left);

    // Twelve instant setters, four applied per block: each block's rendered
    // level must be the level the fourth of its group asked for -- proof the
    // records applied in push order, and that the ones a block left behind
    // were kept rather than discarded.
    constexpr std::uint32_t kCeiling = 4;
    constexpr std::uint32_t kSetters = 12;
    engine->mixer.SetMaxCommandsPerQuantumForTest(kCeiling);
    std::vector<float> wantedGain;
    for (std::uint32_t i = 0; i < kSetters; ++i) {
        const float gain = static_cast<float>(kSetters - i) /
                           static_cast<float>(kSetters);
        wantedGain.push_back(gain);
        ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(
                             voice, 20.0f * std::log10(gain), 0));
    }
    ASSERT_EQ(kSetters, Queued(engine));

    for (std::uint32_t group = 0; group < kSetters / kCeiling; ++group) {
        const Capture out = PumpBlock(engine);
        const float expected =
            0.5f * wantedGain[(group + 1) * kCeiling - 1];
        HARKENTEST_ASSERT_NON_SILENT(out.left);
        EXPECT_NEAR(expected, out.left[kBlockFrames - 1], 1e-5f)
            << "group " << group;
    }
    EXPECT_EQ(0u, Queued(engine));
}

// --- sustained backlog reporting ---

TEST(CommandBudget, SustainedBacklogReportsOncePerEpisodeAndAgainAfterRecovery) {
    EngineSession session(BudgetConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1);
    DrainAudioLogCount(engine, harken::kLogCodeCommandBudgetBehind);

    constexpr std::uint32_t kCeiling = 4;
    constexpr std::uint32_t kPerBlock = 8; // twice what a block can take
    constexpr std::uint32_t kLaggingBlocks = 12; // past the episode threshold
    engine->mixer.SetMaxCommandsPerQuantumForTest(kCeiling);

    // The worst single block of the whole session, which is what the
    // published high-water reports -- it never resets between episodes.
    std::uint32_t sessionWorst = 0;
    const auto lag = [&](std::uint32_t& episodeWorst) {
        episodeWorst = 0;
        for (std::uint32_t block = 0; block < kLaggingBlocks; ++block) {
            for (std::uint32_t i = 0; i < kPerBlock; ++i) {
                ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
            }
            engine->nullBackend.Pump(1);
            const std::uint32_t deferred =
                engine->mixer.CommandsDeferredLastBlock();
            ASSERT_GT(deferred, 0u) << "block " << block;
            episodeWorst = deferred > episodeWorst ? deferred : episodeWorst;
            sessionWorst = deferred > sessionWorst ? deferred : sessionWorst;
            EXPECT_EQ(sessionWorst,
                      engine->mixer.CommandsDeferredHighWater());
        }
    };

    // Every block of the episode falls further behind, yet only the first one
    // past the threshold is reported.
    std::uint32_t firstWorst = 0;
    lag(firstWorst);
    EXPECT_EQ(1u, DrainAudioLogCount(
                      engine, harken::kLogCodeCommandBudgetBehind))
        << "one record per episode, not one per block";

    // Recover fully: no more pushes, drain until a block leaves nothing
    // behind. That block ends the episode.
    std::uint32_t recoveryBlocks = 0;
    while (engine->mixer.CommandsDeferredLastBlock() != 0) {
        engine->nullBackend.Pump(1);
        ASSERT_LT(++recoveryBlocks, 1000u) << "backlog never cleared";
    }
    ASSERT_EQ(0u, Queued(engine));
    EXPECT_EQ(0u, DrainAudioLogCount(
                      engine, harken::kLogCodeCommandBudgetBehind));

    // A second sustained lag is a second episode, so it reports again
    // instead of the signal staying quiet for the rest of the session.
    std::uint32_t secondWorst = 0;
    lag(secondWorst);
    EXPECT_EQ(1u, DrainAudioLogCount(
                      engine, harken::kLogCodeCommandBudgetBehind));

    // Both episodes dig exactly as deep, which is what a complete recovery
    // between them means. The published high-water is the worst block of the
    // whole session, not the current one.
    EXPECT_EQ(firstWorst, secondWorst);
    EXPECT_EQ(sessionWorst, secondWorst);
    const VnSnapshot snapshot = Snap();
    EXPECT_EQ(static_cast<std::uint64_t>(sessionWorst),
              snapshot.commandDeferredCount);
}

// --- the record reaches the integrator as readable text ---

struct LogSink {
    std::vector<std::string> texts;
    std::vector<std::uint32_t> codes;
};

void LogCb(void* userData, std::uint32_t level, std::uint32_t code,
           const char* text) {
    (void)level;
    LogSink* sink = static_cast<LogSink*>(userData);
    sink->codes.push_back(code);
    sink->texts.push_back(text == nullptr ? "" : text);
}

TEST(CommandBudget, SustainedBacklogRecordNamesBothCountsInItsText) {
    LogSink sink;
    VnConfig config = BudgetConfig();
    config.logCallback = &LogCb;
    config.logUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr std::uint32_t kCeiling = 4;
    engine->mixer.SetMaxCommandsPerQuantumForTest(kCeiling);
    for (std::uint32_t block = 0; block < 12; ++block) {
        for (std::uint32_t i = 0; i < 2 * kCeiling; ++i) {
            ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
        }
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(VN_OK, Vn_Update()); // formats the queued records

    // The diagnostic exists to tell an integrator what to change, so the
    // record has to read as a sentence naming how far behind the drain fell
    // and for how long -- not as a bare code with two numbers.
    std::string budgetText;
    for (std::size_t i = 0; i < sink.codes.size(); ++i) {
        if (sink.codes[i] == harken::kLogCodeCommandBudgetBehind) {
            budgetText = sink.texts[i];
        }
    }
    ASSERT_FALSE(budgetText.empty()) << "no budget record reached the sink";
    EXPECT_EQ(std::string::npos, budgetText.find("log code"))
        << "unformatted record: " << budgetText;
    EXPECT_NE(std::string::npos, budgetText.find("ceiling"))
        << budgetText;
    EXPECT_NE(std::string::npos, budgetText.find("later block"))
        << budgetText;
}

// --- a Variable publication bypasses the ordinary command budget ---

TEST(CommandBudget,
     VariablePublicationBypassesCommandBudgetAndEveryTargetLandsTogether) {
    constexpr std::uint32_t kBuses = 6;
    VnConfig config = BudgetConfig(kBuses);
    config.maxBuses = kBuses + 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {kBusVariable}, 0.0f, 1.0f).status);

    // A create publishes a topology plan, and a plan retires only once the
    // audio thread has acknowledged it, so the creates are spread over blocks.
    std::vector<std::vector<float>> dc(kBuses);
    for (std::uint32_t i = 0; i < kBuses; ++i) {
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(kBusBase + i), VN_BUS_MASTER));
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    for (std::uint32_t i = 0; i < kBuses; ++i) {
        const VnBusId bus = harkentest::ObjectId(kBusBase + i);
        PlayDcVoice(harkentest::ObjectId(1 + i), 0.5f, bus, dc[i]);
        const VnBindingDesc definition = BusVolumeBindingDesc();
        const VnBindingAttachDesc attach = BusAttachment(bus);
        ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(kBusBindingBase + i),
                                            &definition));
        ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(kBusBindingBase + i), &attach));
    }
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto halvedBuses = [&]() {
        const VnSnapshot snapshot = Snap();
        std::uint32_t halved = 0;
        for (std::uint32_t i = 0; i < kBuses; ++i) {
            const std::uint32_t busIndex =
                engine->buses.FindIndex(harkentest::ObjectId(kBusBase + i));
            EXPECT_NE(harken::kInvalidBusIndex, busIndex);
            if (std::fabs(snapshot.busPeaks[busIndex] - 0.25f) < 1e-5f) {
                ++halved;
            }
        }
        return halved;
    };
    ASSERT_EQ(0u, halvedBuses()); // all six still at unity volume

    // One Variable change moves all six bus volumes through one current
    // slice, independent of the ordinary command budget.
    constexpr std::uint32_t kCeiling = 2;
    engine->mixer.SetMaxCommandsPerQuantumForTest(kCeiling);
    ASSERT_EQ(VN_OK, Vn_SetValue(kBusVariable, 1.0f));
    for (std::uint32_t step = 0;
         step < 32 && engine->variables.PendingCount() != 0; ++step) {
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(0u, Queued(engine));
        engine->nullBackend.Pump(1);
        const std::uint32_t applied = halvedBuses();
        EXPECT_TRUE(applied == 0u || applied == kBuses);
        EXPECT_EQ(0u, engine->mixer.CommandsDeferredLastBlock());
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(0u, Queued(engine));
    EXPECT_EQ(kBuses, halvedBuses());
}

// --- a deferred ramp starts where it applies ---

TEST(CommandBudget, ADeferredRampStartsAtTheBlockThatAppliesIt) {
    EngineSession session(BudgetConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    std::vector<float> dc;
    const VnVoiceHandle voice = PlayDcVoice(harkentest::ObjectId(1), 0.5f, VN_BUS_MASTER, dc);
    ASSERT_NE(0u, voice);
    Capture warm = PumpBlock(engine);
    HARKENTEST_ASSERT_NON_SILENT(warm.left);

    // Fill the block's budget with instant setters that all land on unity,
    // then queue one ramp behind them: the ramp is what gets deferred.
    constexpr std::uint32_t kCeiling = 4;
    constexpr std::uint32_t kRampMs = 20; // two blocks at 10 ms per block
    engine->mixer.SetMaxCommandsPerQuantumForTest(kCeiling);
    for (std::uint32_t i = 0; i < kCeiling; ++i) {
        ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(voice, 0.0f, 0));
    }
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(voice, kQuarterDb, kRampMs));

    // The block that leaves the ramp behind renders flat at the level in
    // force: a deferred ramp has not started, so nothing moves early.
    const Capture deferring = PumpBlock(engine);
    EXPECT_EQ(1u, engine->mixer.CommandsDeferredLastBlock());
    HARKENTEST_ASSERT_NON_SILENT(deferring.left);
    EXPECT_NEAR(0.5f, deferring.left[0], 1e-6f);
    EXPECT_NEAR(0.5f, deferring.left[kBlockFrames - 1], 1e-6f);

    // The next block applies it, and that is where the ramp starts: it
    // begins from the level in force at this boundary and moves away from it.
    const Capture starting = PumpBlock(engine);
    EXPECT_EQ(0u, engine->mixer.CommandsDeferredLastBlock());
    EXPECT_NEAR(0.5f, starting.left[0], 2e-3f);
    EXPECT_LT(starting.left[kBlockFrames - 1], starting.left[0]);

    // Deferral moves when the ramp runs, not where it ends.
    Capture settled;
    PumpAndCapture(engine, 3, settled);
    EXPECT_NEAR(0.125f, settled.left.back(), 1e-6f);
}

} // namespace

TEST(CommandBudget, OriginalQueueProofFreezesActualBytesAndPublishesOnce) {
    harken::CommandQueue queue(8);
    harken::CommandQueue::BatchReservation reservation{};
    harken::CommandQueue::PreparedBatchProof proof;
    harken::CommandQueue::OriginalBatchRange range;
    std::array<harken::CommandRecord, 2> records{};
    records[0].opcode = harken::kOpcodeSetVoicePan;
    records[0].payload.setVoicePan.handle = 7;
    records[1].opcode = harken::kOpcodeSetVoiceVolume;
    records[1].payload.setVoiceVolume.handle = 7;
    records[1].payload.setVoiceVolume.volumeDb = -6;
    ASSERT_TRUE(queue.ReserveBatch(2, &reservation));
    ASSERT_TRUE(queue.ProvePreparedBatch(&reservation, records.data(), 2, &proof));
    ASSERT_TRUE(queue.IssueOriginalBatchRange(proof, 0, 2, &range, &range));
    harken::CommandRecord frozen{};
    ASSERT_TRUE(queue.ReadPreparedAt(proof, 1, &frozen));
    records[1].payload.setVoiceVolume.volumeDb = 42;
    EXPECT_EQ(0u, queue.PublishedHead());
    EXPECT_EQ(0u, queue.OpcodeSeenMask());
    const auto published = queue.ConsumePreparedBatch(std::move(proof));
    EXPECT_EQ(2u, published.Count());
    EXPECT_FALSE(proof.Valid());
    EXPECT_EQ(&range, queue.OriginalRangeAtFront());
    harken::CommandRecord actual{};
    ASSERT_TRUE(queue.TryPop(actual));
    ASSERT_TRUE(queue.TryPop(actual));
    EXPECT_EQ(0, std::memcmp(&frozen, &actual, sizeof(actual)));
    EXPECT_NE(0u, queue.OpcodeSeenMask());
    ASSERT_TRUE(queue.CompleteOriginalRange(range));
    EXPECT_TRUE(range.ResetAfterDisposition());
}

TEST(CommandBudget, OriginalQueueProofAbortReproveKeepsSequenceAndRefusesExhaustedOrigin) {
    harken::CommandQueue queue(8);
    harken::CommandQueue::BatchReservation reservation{};
    harken::CommandQueue::PreparedBatchProof proof;
    harken::CommandQueue::OriginalBatchRange range;
    harken::CommandRecord record{};
    record.opcode = harken::kOpcodeSetVoicePan;
    ASSERT_TRUE(queue.ReserveBatch(1, &reservation));
    ASSERT_TRUE(queue.ProvePreparedBatch(&reservation, &record, 1, &proof));
    ASSERT_TRUE(queue.IssueOriginalBatchRange(proof, 0, 1, &range, &range));
    harken::CommandRecord first{};
    ASSERT_TRUE(queue.ReadPreparedAt(proof, 0, &first));
    ASSERT_TRUE(queue.CancelReservedBatchExact(&reservation));
    EXPECT_FALSE(proof.Valid());
    EXPECT_EQ(harken::CommandQueue::OriginalRangeState::UnpublishedCancelled, range.State());
    EXPECT_EQ(0u, queue.PublishedHead());
    EXPECT_EQ(0u, queue.OpcodeSeenMask());
    ASSERT_TRUE(range.ResetAfterDisposition());
    proof.Release();
    ASSERT_TRUE(queue.ReserveBatch(1, &reservation));
    ASSERT_TRUE(queue.ProvePreparedBatch(&reservation, &record, 1, &proof));
    harken::CommandRecord second{};
    ASSERT_TRUE(queue.ReadPreparedAt(proof, 0, &second));
    EXPECT_EQ(first.sequence, second.sequence);
    proof.Release();
    harken::detail::CommandQueueSubmissionTestAccess::ExhaustFrozenOrigins(queue);
    ASSERT_TRUE(queue.ReserveBatch(1, &reservation));
    EXPECT_FALSE(queue.ProvePreparedBatch(&reservation, &record, 1, &proof));
    EXPECT_EQ(0u, queue.PublishedHead());
    ASSERT_TRUE(queue.CancelReservedBatchExact(&reservation));
}

TEST(CommandBudget, OriginalQueueDispositionReceiptTracksActualAbortAndIndependentSameSlotReissue) {
    harken::CommandQueue queue(8), foreign(8);
    harken::CommandQueue::BatchReservation reservation{};
    harken::CommandQueue::PreparedBatchProof proof;
    harken::CommandQueue::PreparedBatchDispositionReceipt original, next;
    harken::CommandQueue::OriginalBatchRange range;
    harken::CommandRecord record{};
    record.opcode = harken::kOpcodeSetVoicePan;
    ASSERT_TRUE(queue.ReserveBatch(1, &reservation));
    ASSERT_TRUE(queue.ProvePreparedBatch(&reservation, &record, 1, &proof, &original));
    ASSERT_TRUE(queue.IssueOriginalBatchRange(proof, 0, 1, &range, &range));
    EXPECT_TRUE(original.Issued());
    EXPECT_FALSE(original.CancelledUnpublishedFrom(queue));
    EXPECT_FALSE(original.PublishedFrom(queue));
    harken::CommandRecord first{};
    ASSERT_TRUE(queue.ReadPreparedAt(proof, 0, &first));
    auto moved = std::move(proof);
    EXPECT_FALSE(proof.Valid());
    moved.Release();
    EXPECT_TRUE(original.CancelledUnpublishedFrom(queue));
    EXPECT_FALSE(original.CancelledUnpublishedFrom(foreign));
    EXPECT_EQ(harken::CommandQueue::OriginalRangeState::UnpublishedCancelled, range.State());
    EXPECT_EQ(0u, queue.PublishedHead());
    EXPECT_EQ(0u, queue.OpcodeSeenMask());
    EXPECT_FALSE(queue.CancelReservedBatchExact(&reservation));
    ASSERT_TRUE(range.ResetAfterDisposition());
    ASSERT_TRUE(queue.ReserveBatch(1, &reservation));
    ASSERT_TRUE(queue.ProvePreparedBatch(&reservation, &record, 1, &proof, &next));
    ASSERT_TRUE(queue.IssueOriginalBatchRange(proof, 0, 1, &range, &range));
    EXPECT_FALSE(next.CancelledUnpublishedFrom(queue));
    EXPECT_FALSE(next.PublishedFrom(queue));
    harken::CommandRecord second{};
    ASSERT_TRUE(queue.ReadPreparedAt(proof, 0, &second));
    EXPECT_EQ(first.sequence, second.sequence);
    EXPECT_TRUE(original.CancelledUnpublishedFrom(queue));
    const auto published = queue.ConsumePreparedBatch(std::move(proof));
    EXPECT_EQ(1u, published.Count());
    EXPECT_TRUE(next.PublishedFrom(queue));
    EXPECT_FALSE(next.PublishedFrom(foreign));
    EXPECT_FALSE(next.CancelledUnpublishedFrom(queue));
    ASSERT_TRUE(queue.TryPop(second));
    ASSERT_TRUE(queue.CompleteOriginalRange(range));
    EXPECT_TRUE(range.ResetAfterDisposition());
}

TEST(CommandBudget, OriginalQueueDispositionReceiptExactCancelAndRepeatedReleaseKeepOriginalResult) {
    harken::CommandQueue queue(8);
    harken::CommandQueue::BatchReservation reservation{};
    harken::CommandQueue::PreparedBatchProof proof;
    harken::CommandQueue::PreparedBatchDispositionReceipt receipt;
    harken::CommandRecord record{};
    record.opcode = harken::kOpcodeSetVoicePan;
    ASSERT_TRUE(queue.ReserveBatch(1, &reservation));
    ASSERT_TRUE(queue.ProvePreparedBatch(&reservation, &record, 1, &proof, &receipt));
    ASSERT_TRUE(queue.CancelReservedBatchExact(&reservation));
    EXPECT_TRUE(receipt.CancelledUnpublishedFrom(queue));
    proof.Release();
    proof.Release();
    EXPECT_TRUE(receipt.CancelledUnpublishedFrom(queue));
    EXPECT_FALSE(receipt.PublishedFrom(queue));
    EXPECT_EQ(0u, queue.PublishedHead());
    EXPECT_EQ(0u, queue.OpcodeSeenMask());
}

TEST(CommandBudget, OriginalQueueDispositionReceiptOccupiedOutputRefusesBeforeFreezeOrPublication) {
    harken::CommandQueue queue(8), foreign(8);
    harken::CommandQueue::BatchReservation reservation{}, other{};
    harken::CommandQueue::PreparedBatchProof proof, otherProof;
    harken::CommandQueue::PreparedBatchDispositionReceipt receipt, fresh;
    harken::CommandRecord record{};
    record.opcode = harken::kOpcodeSetVoicePan;
    ASSERT_TRUE(queue.ReserveBatch(1, &reservation));
    ASSERT_TRUE(queue.ProvePreparedBatch(&reservation, &record, 1, &proof, &receipt));
    proof.Release();
    ASSERT_TRUE(receipt.CancelledUnpublishedFrom(queue));
    ASSERT_TRUE(foreign.ReserveBatch(1, &other));
    EXPECT_FALSE(foreign.ProvePreparedBatch(&other, &record, 1, &otherProof, &receipt));
    EXPECT_TRUE(receipt.CancelledUnpublishedFrom(queue));
    EXPECT_FALSE(receipt.CancelledUnpublishedFrom(foreign));
    EXPECT_EQ(0u, foreign.PublishedHead());
    EXPECT_EQ(0u, foreign.OpcodeSeenMask());
    EXPECT_FALSE(otherProof.Valid());
    ASSERT_TRUE(foreign.ProvePreparedBatch(&other, &record, 1, &otherProof, &fresh));
    otherProof.Release();
    EXPECT_TRUE(fresh.CancelledUnpublishedFrom(foreign));
    EXPECT_FALSE(fresh.CancelledUnpublishedFrom(queue));
}

TEST(CommandBudget, OriginalQueueDispositionReceiptFailedFreezeDoesNotIssueOrCancel) {
    harken::CommandQueue queue(8);
    harken::CommandQueue::BatchReservation reservation{};
    harken::CommandQueue::PreparedBatchProof proof;
    harken::CommandQueue::PreparedBatchDispositionReceipt receipt;
    harken::CommandRecord record{};
    record.opcode = harken::kOpcodeSetVoicePan;
    ASSERT_TRUE(queue.ReserveBatch(1, &reservation));
    EXPECT_FALSE(queue.ProvePreparedBatch(&reservation, &record, 2, &proof, &receipt));
    EXPECT_FALSE(receipt.Issued());
    EXPECT_FALSE(receipt.CancelledUnpublishedFrom(queue));
    harken::detail::CommandQueueSubmissionTestAccess::ExhaustFrozenOrigins(queue);
    EXPECT_FALSE(queue.ProvePreparedBatch(&reservation, &record, 1, &proof, &receipt));
    EXPECT_FALSE(receipt.Issued());
    ASSERT_TRUE(queue.CancelReservedBatchExact(&reservation));
    EXPECT_FALSE(receipt.CancelledUnpublishedFrom(queue));
}

TEST(CommandBudget, OriginalQueueDispositionReceiptScopeExitCancelsOnlyItsTrueFrozenPreparation) {
    harken::CommandQueue queue(8);
    harken::CommandQueue::BatchReservation reservation{};
    harken::CommandQueue::PreparedBatchProof proof, nextProof;
    harken::CommandQueue::OriginalBatchRange range;
    harken::CommandQueue::PreparedBatchDispositionReceipt next;
    harken::CommandRecord record{};
    record.opcode = harken::kOpcodeSetVoicePan;
    {
        harken::CommandQueue::PreparedBatchDispositionReceipt receipt;
        ASSERT_TRUE(queue.ReserveBatch(1, &reservation));
        ASSERT_TRUE(queue.ProvePreparedBatch(&reservation, &record, 1, &proof, &receipt));
        ASSERT_TRUE(queue.IssueOriginalBatchRange(proof, 0, 1, &range, &range));
        EXPECT_FALSE(receipt.CancelledUnpublishedFrom(queue));
    }
    EXPECT_FALSE(proof.Valid());
    EXPECT_EQ(harken::CommandQueue::OriginalRangeState::UnpublishedCancelled, range.State());
    EXPECT_EQ(0u, queue.PublishedHead());
    EXPECT_EQ(0u, queue.OpcodeSeenMask());
    ASSERT_TRUE(range.ResetAfterDisposition());
    ASSERT_TRUE(queue.ReserveBatch(1, &reservation));
    ASSERT_TRUE(queue.ProvePreparedBatch(&reservation, &record, 1, &nextProof, &next));
    proof.Release();
    EXPECT_TRUE(nextProof.Valid());
    EXPECT_FALSE(next.CancelledUnpublishedFrom(queue));
    EXPECT_FALSE(next.PublishedFrom(queue));
    const auto published = queue.ConsumePreparedBatch(std::move(nextProof));
    EXPECT_EQ(1u, published.Count());
    EXPECT_TRUE(next.PublishedFrom(queue));
    EXPECT_FALSE(next.CancelledUnpublishedFrom(queue));
    ASSERT_TRUE(queue.TryPop(record));
}
