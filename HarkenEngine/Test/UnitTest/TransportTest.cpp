// Command/notification transport behavior: SPSC structural guarantees, command
// backpressure with zero side effects and successful retry after a drain,
// completion notifications for all three types, callback delivery confined to
// Vn_Update, and the notification ring's drop-oldest policy with its counter.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/core/ChannelLayout.h"
#include "harken/core/CommandQueue.h"
#include "harken/core/NotificationQueue.h"
#include "harken/param/ParamTargets.h"
#include "harken/render/BuiltinEffects.h"

#include <atomic>
#include <bit>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;

static_assert(harken::kCommandVersion == 7,
              "Action Seek requires transport version 7");

// Structural contract of the transport records and queues.
static_assert(sizeof(harken::CommandRecord) == 64,
              "command records are fixed 64-byte POD");
static_assert(sizeof(harken::PlayLedBatchHeader) == 16,
              "play-led batch header layout is frozen");
static_assert(std::is_trivial_v<harken::PlayLedBatchHeader> &&
                  std::is_standard_layout_v<harken::PlayLedBatchHeader>,
              "play-led batch header stays a trivial wire value");
static_assert(sizeof(harken::CommandRecord::Payload::Play) <= 56,
              "play-led commands fit the fixed payload");
static_assert(sizeof(harken::CommandRecord::Payload::Stop) <= 56,
              "stop-led commands fit the fixed payload");
static_assert(offsetof(harken::CommandRecord::Payload::Play, batch) == 0,
              "play-led header starts the payload");
static_assert(sizeof(harken::PreparedBiquadCoeffs) == 20,
              "prepared coefficient payload is five floats");
static_assert(sizeof(harken::CommandRecord::Payload::SetVoiceFilter) == 56,
              "prepared voice-filter payload fits the fixed record");
static_assert(sizeof(harken::CommandRecord::Payload::SetVoiceLowShelf) == 56,
              "prepared voice-shelf payload fits the fixed record");
static_assert(
    sizeof(harken::CommandRecord::Payload::SetVoiceEffectParam) == 56,
    "prepared voice-effect payload fits the fixed record");
static_assert(sizeof(harken::CommandRecord::Payload::SetBusEffectParam) == 52,
              "prepared bus-effect payload fits the fixed record");
static_assert(
    harken::kCommandKnownFlagsMask ==
        (harken::kCommandFlagPreparedCompressor |
         harken::kCommandFlagVariableWeightSegment) &&
        harken::kCommandFlagPreparedCompressor == 0x01u &&
        harken::kCommandFlagVariableWeightSegment == 0x02u,
    "prepared compressor and variable weight own distinct flags");
static_assert(
    sizeof(harken::CommandRecord::Payload::SetCompressorEffectParam) == 56,
    "prepared compressor payload fills the fixed record");
static_assert(
    offsetof(harken::CommandRecord::Payload::SetCompressorEffectParam,
             host) == 0 &&
        offsetof(
            harken::CommandRecord::Payload::SetCompressorEffectParam,
            slot) == 8 &&
        offsetof(
            harken::CommandRecord::Payload::SetCompressorEffectParam,
            effectType) == 12 &&
        offsetof(
            harken::CommandRecord::Payload::SetCompressorEffectParam,
            transitionFrames) == 16 &&
        offsetof(
            harken::CommandRecord::Payload::SetCompressorEffectParam,
            thresholdDb) == 20 &&
        offsetof(
            harken::CommandRecord::Payload::SetCompressorEffectParam,
            hardKneeScale) == 52,
    "prepared compressor payload offsets fill exactly 56 bytes");
static_assert(
    std::is_trivially_copyable_v<
        harken::CommandRecord::Payload::SetCompressorEffectParam>,
    "prepared compressor payload must be trivially copyable");
static_assert(
    offsetof(harken::CommandRecord::Payload::SetVoiceEffectParam,
             transitionFrames) == 48 &&
        offsetof(harken::CommandRecord::Payload::SetBusEffectParam,
                 transitionFrames) == 44,
    "effect transition duration appends after the frozen payload fields");
static_assert(std::is_trivially_copyable_v<harken::CommandRecord>,
              "command records must be trivially copyable");
static_assert(sizeof(harken::CommandRecord::Payload::SetVoiceSpatialScalar) ==
                  16,
              "spatial scalar payload layout is frozen");
static_assert(harken::kOpcodeSetVoiceWidth == 9 &&
                  harken::kOpcodeSetVoiceWidth ==
                      harken::kOpcodeSetVoiceSpatialScalar,
              "width keeps the frozen opcode-9 alias");
static_assert(sizeof(harken::CommandRecord::Payload::SetVoiceWidth) == 16,
              "width alias payload layout is frozen");
static_assert(offsetof(harken::CommandRecord::Payload::SetVoiceSpatialScalar,
                       handle) == 0,
              "spatial scalar handle offset drift");
static_assert(offsetof(harken::CommandRecord::Payload::SetVoiceSpatialScalar,
                       value) == 8,
              "spatial scalar value offset drift");
static_assert(offsetof(harken::CommandRecord::Payload::SetVoiceSpatialScalar,
                       kind) == 12,
              "spatial scalar kind offset drift");
static_assert(std::is_trivially_copyable_v<VnNotification>,
              "notification records must be trivially copyable");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "SPSC indices must be lock-free atomics");
static_assert(noexcept(
    std::declval<harken::CommandQueue&>().CommitReservedBatch(nullptr, nullptr)));
static_assert(noexcept(
    std::declval<harken::CommandQueue&>().CancelReservedBatch(nullptr)));

struct NotificationLog {
    std::vector<VnNotification> notifications;
};

void RecordNotification(const VnNotification* notification, void* userData) {
    static_cast<NotificationLog*>(userData)->notifications.push_back(*notification);
}

VnConfig ConfigWithLog(NotificationLog* log, std::uint32_t maxVoices,
                       std::uint32_t notificationQueueDepth = 0,
                       std::uint32_t commandQueueDepth = 0) {
    VnConfig config = MakeNullConfig(maxVoices, kBlockFrames);
    config.onNotification = &RecordNotification;
    config.notificationUserData = log;
    config.notificationQueueDepth = notificationQueueDepth;
    config.commandQueueDepth = commandQueueDepth;
    return config;
}

void ExpectCoeffsEqual(const harken::BiquadCoeffs& expected,
                       const harken::PreparedBiquadCoeffs& actual) {
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.b0),
              std::bit_cast<std::uint32_t>(actual.b0));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.b1),
              std::bit_cast<std::uint32_t>(actual.b1));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.b2),
              std::bit_cast<std::uint32_t>(actual.b2));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.a1),
              std::bit_cast<std::uint32_t>(actual.a1));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.a2),
              std::bit_cast<std::uint32_t>(actual.a2));
}

VnThreeBandToneDesc ToneDesc(float lowGain, float lowFreq, float peakGain,
                             float peakFreq, float peakQ, float highGain,
                             float highFreq) {
    VnThreeBandToneDesc desc{};
    desc.structSize = sizeof(VnThreeBandToneDesc);
    desc.lowShelfGainDb = lowGain;
    desc.lowShelfFreqHz = lowFreq;
    desc.peakGainDb = peakGain;
    desc.peakFreqHz = peakFreq;
    desc.peakQ = peakQ;
    desc.highShelfGainDb = highGain;
    desc.highShelfFreqHz = highFreq;
    return desc;
}

const VnEffectDesc* AsEffectDesc(const void* desc) {
    return static_cast<const VnEffectDesc*>(desc);
}

TEST(Transport, ResolvedPanCommandRetainsRecordSizeAndValues) {
    constexpr float pan = 0.5f;
    const harken::ResolvedPanGains resolved =
        harken::ResolvePanGains(VN_CHANNEL_LAYOUT_5_1, pan);
    harken::CommandRecord record{};
    record.opcode = harken::kOpcodeSetVoicePan;
    record.payload.setVoicePan.handle = 7;
    record.payload.setVoicePan.pan = resolved.pan;
    record.payload.setVoicePan.pan2d = resolved.pan;
    record.payload.setVoicePan.gain0 = resolved.gain0;
    record.payload.setVoicePan.gain1 = resolved.gain1;
    record.payload.setVoicePan.channel0 = resolved.channel0;
    record.payload.setVoicePan.channel1 = resolved.channel1;

    harken::CommandQueue queue(1);
    ASSERT_TRUE(queue.TryPush(record));
    harken::CommandRecord out{};
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(7u, out.payload.setVoicePan.handle);
    EXPECT_FLOAT_EQ(resolved.pan, out.payload.setVoicePan.pan);
    EXPECT_FLOAT_EQ(resolved.gain0, out.payload.setVoicePan.gain0);
    EXPECT_FLOAT_EQ(resolved.gain1, out.payload.setVoicePan.gain1);
    EXPECT_EQ(resolved.channel0, out.payload.setVoicePan.channel0);
    EXPECT_EQ(resolved.channel1, out.payload.setVoicePan.channel1);
}

TEST(Transport, WidthThenMixCommandsKeepOpcodeNineFifoOrder) {
    EngineSession session(MakeNullConfig(1, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -2.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);
    const VnVoiceHandle render =
        harkentest::RenderHandle(session.Engine(), handle);

    harken::CommandRecord width{};
    width.opcode = harken::kOpcodeSetVoiceSpatialScalar;
    width.payload.setVoiceWidth.handle = render;
    width.payload.setVoiceWidth.width = 0.25f;
    harken::CommandRecord mix{};
    mix.opcode = harken::kOpcodeSetVoiceSpatialScalar;
    mix.payload.setVoiceSpatialScalar.handle = render;
    mix.payload.setVoiceSpatialScalar.value = 0.5f;
    mix.payload.setVoiceSpatialScalar.kind =
        harken::kSpatialScalarKindSpatializationMix;
    ASSERT_TRUE(session.Engine()->commands.TryPush(width));
    ASSERT_TRUE(session.Engine()->commands.TryPush(mix));
    session.Engine()->nullBackend.Pump(1);

    const harken::VoiceRenderContext* context =
        session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    EXPECT_FLOAT_EQ(0.25f, context->width);
    EXPECT_FLOAT_EQ(0.5f, context->spatializationMix);
}

TEST(Transport, UnknownSpatialScalarKindIsDefensiveNoOp) {
    EngineSession session(MakeNullConfig(1, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -2.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);
    const VnVoiceHandle render =
        harkentest::RenderHandle(session.Engine(), handle);
    harken::CommandRecord command{};
    command.opcode = harken::kOpcodeSetVoiceSpatialScalar;
    command.payload.setVoiceSpatialScalar.handle = render;
    command.payload.setVoiceSpatialScalar.value = 0.0f;
    command.payload.setVoiceSpatialScalar.kind = 99u;
    ASSERT_TRUE(session.Engine()->commands.TryPush(command));
    session.Engine()->nullBackend.Pump(1);
    const harken::VoiceRenderContext* context =
        session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    EXPECT_FLOAT_EQ(1.0f, context->spatializationMix);
    EXPECT_FLOAT_EQ(0.0f, context->width);
}

TEST(Transport, VoiceFilterAndShelfCommandsCarryPreparedDesign) {
    constexpr std::uint32_t sampleRate = 48000;
    EngineSession session(MakeNullConfig(1, kBlockFrames, sampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> dc = harkentest::MakeDC(sampleRate, 0.25f);
    const VnPCMDesc pcm = MonoDesc(dc, sampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);
    const VnVoiceHandle render =
        harkentest::RenderHandle(session.Engine(), handle);

    constexpr float cutoff = 1200.0f;
    session.Engine()->params.SetAttenuationSlot(
        render, harken::kTargetVoiceLowpassCutoff, cutoff);
    ASSERT_EQ(VN_OK, Vn_Update());
    harken::CommandRecord command{};
    ASSERT_TRUE(session.Engine()->commands.TryPop(command));
    ASSERT_EQ(harken::kOpcodeSetVoiceFilter, command.opcode);
    EXPECT_EQ(harken::kFilterKindLowpass,
              command.payload.setVoiceFilter.filterKind);
    EXPECT_EQ(1u, command.payload.setVoiceFilter.enable);
    EXPECT_FLOAT_EQ(harken::kToneDefaultQ,
                    command.payload.setVoiceFilter.q);
    const harken::BiquadCoeffs lowpass = harken::MakeLowpass(
        static_cast<float>(sampleRate), cutoff, 0.70710678f);
    ExpectCoeffsEqual(lowpass, command.payload.setVoiceFilter.coeffs);
    EXPECT_EQ(harken::EstimateBiquadTailFrames(lowpass),
              command.payload.setVoiceFilter.tailFrames);

    constexpr float shelfGain = 6.0f;
    session.Engine()->params.SetAttenuationSlot(
        render, harken::kTargetVoiceLowShelfGainDb, shelfGain);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_TRUE(session.Engine()->commands.TryPop(command));
    ASSERT_EQ(harken::kOpcodeSetVoiceLowShelf, command.opcode);
    EXPECT_EQ(harken::kShelfKindLow,
              command.payload.setVoiceShelf.shelfKind);
    EXPECT_FLOAT_EQ(harken::kToneLowShelfDefaultHz,
                    command.payload.setVoiceShelf.frequencyHz);
    EXPECT_FLOAT_EQ(harken::kToneDefaultQ,
                    command.payload.setVoiceShelf.q);
    const harken::BiquadCoeffs shelf = harken::MakeLowShelf(
        static_cast<float>(sampleRate), 250.0f, 0.70710678f, shelfGain);
    ExpectCoeffsEqual(shelf, command.payload.setVoiceLowShelf.coeffs);
    EXPECT_EQ(harken::EstimateBiquadTailFrames(shelf),
              command.payload.setVoiceLowShelf.tailFrames);
    EXPECT_FALSE(session.Engine()->commands.TryPop(command));
}

TEST(Transport, ToneParamCommandsCarryOnlyTheAffectedPreparedSection) {
    constexpr std::uint32_t sampleRate = 48000;
    {
        VnConfig config = MakeNullConfig(1, kBlockFrames, sampleRate);
        config.fxSlotsPerVoice = 1;
        config.toneEffectPoolCapacity = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());

        const std::vector<float> dc = harkentest::MakeDC(sampleRate, 0.25f);
        const VnPCMDesc pcm = MonoDesc(dc, sampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        session.Engine()->nullBackend.Pump(1);

        const VnThreeBandToneDesc tone =
            ToneDesc(3.0f, 180.0f, 5.0f, 1400.0f, 1.25f, -2.0f, 7200.0f);
        ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
                             handle, 0, VN_EFFECT_THREE_BAND_TONE,
                             AsEffectDesc(&tone)));
        session.Engine()->nullBackend.Pump(1);

        constexpr float peakGain = 7.0f;
        ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(
                             handle, 0, harken::kToneParamPeakGainDb,
                             peakGain));
        constexpr float peakQ = 2.5f;
        ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(
                             handle, 0, harken::kToneParamPeakQ, peakQ));
        harken::CommandRecord command{};
        ASSERT_TRUE(session.Engine()->commands.TryPop(command));
        ASSERT_EQ(harken::kOpcodeSetVoiceEffectParam, command.opcode);
        ASSERT_EQ(harken::kToneParamPeakGainDb,
                  command.payload.setVoiceEffectParam.paramIndex);
        ASSERT_TRUE(session.Engine()->commands.TryPop(command));
        ASSERT_EQ(harken::kOpcodeSetVoiceEffectParam, command.opcode);
        EXPECT_EQ(VN_EFFECT_THREE_BAND_TONE,
                  command.payload.setVoiceEffectParam.effectType);
        EXPECT_EQ(harken::kToneParamPeakQ,
                  command.payload.setVoiceEffectParam.paramIndex);
        const harken::BiquadCoeffs peak = harken::MakePeaking(
            static_cast<float>(sampleRate), tone.peakFreqHz, peakQ,
            peakGain);
        ExpectCoeffsEqual(peak,
                          command.payload.setVoiceEffectParam.coeffs);
        EXPECT_EQ(harken::EstimateBiquadTailFrames(peak),
                  command.payload.setVoiceEffectParam.tailFrames);
    }
    {
        VnConfig config = MakeNullConfig(0, kBlockFrames, sampleRate);
        config.fxSlotsPerBus = 1;
        config.toneEffectPoolCapacity = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());

        const VnThreeBandToneDesc tone =
            ToneDesc(2.0f, 220.0f, -4.0f, 900.0f, 0.8f, 1.0f, 6500.0f);
        ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                             VN_BUS_MASTER, 0, VN_EFFECT_THREE_BAND_TONE,
                             AsEffectDesc(&tone)));
        session.Engine()->nullBackend.Pump(1);

        constexpr float highFreq = 8200.0f;
        ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(
                             VN_BUS_MASTER, 0,
                             harken::kToneParamHighShelfFreqHz, highFreq));
        constexpr float highGain = 7.0f;
        ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(
                             VN_BUS_MASTER, 0,
                             harken::kToneParamHighShelfGainDb, highGain));
        harken::CommandRecord command{};
        ASSERT_TRUE(session.Engine()->commands.TryPop(command));
        ASSERT_EQ(harken::kOpcodeSetBusEffectParam, command.opcode);
        ASSERT_EQ(harken::kToneParamHighShelfFreqHz,
                  command.payload.setBusEffectParam.paramIndex);
        ASSERT_TRUE(session.Engine()->commands.TryPop(command));
        ASSERT_EQ(harken::kOpcodeSetBusEffectParam, command.opcode);
        EXPECT_EQ(VN_EFFECT_THREE_BAND_TONE,
                  command.payload.setBusEffectParam.effectType);
        EXPECT_EQ(harken::kToneParamHighShelfGainDb,
                  command.payload.setBusEffectParam.paramIndex);
        const harken::BiquadCoeffs highShelf = harken::MakeHighShelf(
            static_cast<float>(sampleRate), highFreq,
            harken::kToneShelfQ, highGain);
        ExpectCoeffsEqual(highShelf,
                          command.payload.setBusEffectParam.coeffs);
        EXPECT_EQ(harken::EstimateBiquadTailFrames(highShelf),
                  command.payload.setBusEffectParam.tailFrames);
    }
}

TEST(Transport, FixedTonePublishesOneCompleteCommandPerPhysicalSection) {
    constexpr std::uint32_t sampleRate = 48000;
    VnConfig config = MakeNullConfig(1, kBlockFrames, sampleRate);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc =
        harkentest::MakeDC(sampleRate, 0.125f);
    const VnPCMDesc pcm = MonoDesc(dc, sampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);
    const VnVoiceHandle render =
        harkentest::RenderHandle(session.Engine(), handle);

    session.Engine()->params.SetAttenuationSlot(
        render, harken::kTargetVoiceLowpassCutoff, 1200.0f);
    session.Engine()->params.SetAttenuationSlot(
        render, harken::kTargetVoiceLowpassQ, 1.0f);
    session.Engine()->params.SetAttenuationSlot(
        render, harken::kTargetVoiceHighpassCutoff, 80.0f);
    session.Engine()->params.SetAttenuationSlot(
        render, harken::kTargetVoiceHighpassQ, 1.0f);
    session.Engine()->params.SetAttenuationSlot(
        render, harken::kTargetVoiceLowShelfGainDb, 6.0f);
    session.Engine()->params.SetAttenuationSlot(
        render, harken::kTargetVoiceLowShelfFreqHz, 1.0f);
    session.Engine()->params.SetAttenuationSlot(
        render, harken::kTargetVoiceLowShelfQ, 1.0f);
    session.Engine()->params.SetAttenuationSlot(
        render, harken::kTargetVoiceHighShelfGainDb, -6.0f);
    session.Engine()->params.SetAttenuationSlot(
        render, harken::kTargetVoiceHighShelfFreqHz, -1.0f);
    session.Engine()->params.SetAttenuationSlot(
        render, harken::kTargetVoiceHighShelfQ, 1.0f);
    ASSERT_EQ(VN_OK, Vn_Update());

    harken::CommandRecord commands[4]{};
    for (harken::CommandRecord& command : commands) {
        ASSERT_TRUE(session.Engine()->commands.TryPop(command));
    }
    harken::CommandRecord extra{};
    EXPECT_FALSE(session.Engine()->commands.TryPop(extra));
    EXPECT_EQ(harken::kOpcodeSetVoiceFilter, commands[0].opcode);
    EXPECT_EQ(harken::kFilterKindLowpass,
              commands[0].payload.setVoiceFilter.filterKind);
    EXPECT_FLOAT_EQ(1200.0f,
                    commands[0].payload.setVoiceFilter.cutoffHz);
    EXPECT_FLOAT_EQ(harken::kToneDefaultQ * 2.0f,
                    commands[0].payload.setVoiceFilter.q);
    EXPECT_EQ(harken::kOpcodeSetVoiceFilter, commands[1].opcode);
    EXPECT_EQ(harken::kFilterKindHighpass,
              commands[1].payload.setVoiceFilter.filterKind);
    EXPECT_EQ(harken::kOpcodeSetVoiceShelf, commands[2].opcode);
    EXPECT_EQ(harken::kShelfKindLow,
              commands[2].payload.setVoiceShelf.shelfKind);
    EXPECT_FLOAT_EQ(500.0f,
                    commands[2].payload.setVoiceShelf.frequencyHz);
    EXPECT_EQ(harken::kOpcodeSetVoiceShelf, commands[3].opcode);
    EXPECT_EQ(harken::kShelfKindHigh,
              commands[3].payload.setVoiceShelf.shelfKind);
    EXPECT_FLOAT_EQ(3000.0f,
                    commands[3].payload.setVoiceShelf.frequencyHz);
}

TEST(Transport, CommandQueueFifoOrderAndCapacityRounding) {
    harken::CommandQueue queue(3); // rounds up to the next power of two
    EXPECT_EQ(4u, queue.Capacity());

    for (std::uint16_t i = 0; i < 4; ++i) {
        harken::CommandRecord record{};
        // In-range opcodes as distinguishable FIFO markers (the queue asserts
        // the frozen vocabulary at push); FIFO order is checked below by both
        // the opcode and the producer sequence stamp.
        record.opcode = static_cast<std::uint16_t>(harken::kOpcodePlay + i);
        EXPECT_TRUE(queue.TryPush(record)) << "push " << i;
    }
    EXPECT_TRUE(queue.Full());

    harken::CommandRecord overflow{};
    overflow.opcode = harken::kOpcodePlay; // valid; refused by capacity, not vocabulary
    EXPECT_FALSE(queue.TryPush(overflow));
    EXPECT_EQ(1u, queue.BackpressureCount());

    harken::CommandRecord out{};
    for (std::uint16_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(queue.TryPop(out)) << "pop " << i;
        EXPECT_EQ(static_cast<std::uint16_t>(harken::kOpcodePlay + i),
                  out.opcode);             // strict FIFO
        EXPECT_EQ(i, out.sequence);        // producer stamps monotonically
        EXPECT_EQ(harken::kCommandVersion, out.version);
    }
    EXPECT_FALSE(queue.TryPop(out));

    // Space freed by the drain admits new pushes.
    harken::CommandRecord again{};
    again.opcode = harken::kOpcodePlay;
    EXPECT_TRUE(queue.TryPush(again));
}

TEST(Transport, CommandQueueBatchHandlesEmptyNullAndExactFit) {
    harken::CommandQueue queue(4);
    EXPECT_TRUE(queue.TryPushBatch(nullptr, 0));
    EXPECT_FALSE(queue.TryPushBatch(nullptr, 1));
    EXPECT_EQ(0u, queue.BackpressureCount());

    harken::CommandRecord batch[4]{};
    for (std::uint16_t i = 0; i < 4; ++i) {
        batch[i].opcode =
            static_cast<std::uint16_t>(harken::kOpcodePlay + i);
    }
    ASSERT_TRUE(queue.TryPushBatch(batch, 4));
    EXPECT_TRUE(queue.Full());

    harken::CommandRecord out{};
    for (std::uint16_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(queue.TryPop(out)) << "pop " << i;
        EXPECT_EQ(static_cast<std::uint16_t>(harken::kOpcodePlay + i),
                  out.opcode);
        EXPECT_EQ(i, out.sequence);
        EXPECT_EQ(harken::kCommandVersion, out.version);
    }
    EXPECT_FALSE(queue.TryPop(out));
}

TEST(Transport, CommandQueueBatchOneSlotShortPublishesNothing) {
    harken::CommandQueue queue(4);
    harken::CommandRecord existing{};
    existing.opcode = harken::kOpcodePlay;
    ASSERT_TRUE(queue.TryPush(existing));

    harken::CommandRecord batch[4]{};
    for (std::uint16_t i = 0; i < 4; ++i) {
        batch[i].opcode =
            static_cast<std::uint16_t>(harken::kOpcodeStop + i);
    }
    EXPECT_FALSE(queue.TryPushBatch(batch, 4));
    EXPECT_EQ(1u, queue.BackpressureCount());

    harken::CommandRecord out{};
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(harken::kOpcodePlay, out.opcode);
    EXPECT_EQ(0u, out.sequence);
    EXPECT_EQ(harken::kCommandVersion, out.version);
    EXPECT_FALSE(queue.TryPop(out));

    harken::CommandRecord after{};
    after.opcode = harken::kOpcodeStop;
    ASSERT_TRUE(queue.TryPush(after));
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(harken::kOpcodeStop, out.opcode);
    EXPECT_EQ(1u, out.sequence);
    EXPECT_EQ(harken::kCommandVersion, out.version);
}

TEST(Transport, CommandQueueBatchWrapKeepsFifoAndConsecutiveSequences) {
    harken::CommandQueue queue(4);
    harken::CommandRecord first[3]{};
    for (std::uint16_t i = 0; i < 3; ++i) {
        first[i].opcode =
            static_cast<std::uint16_t>(harken::kOpcodePlay + i);
    }
    ASSERT_TRUE(queue.TryPushBatch(first, 3));

    harken::CommandRecord out{};
    for (std::uint16_t i = 0; i < 2; ++i) {
        ASSERT_TRUE(queue.TryPop(out));
        EXPECT_EQ(static_cast<std::uint16_t>(harken::kOpcodePlay + i),
                  out.opcode);
        EXPECT_EQ(i, out.sequence);
        EXPECT_EQ(harken::kCommandVersion, out.version);
    }

    harken::CommandRecord wrapped[3]{};
    for (std::uint16_t i = 0; i < 3; ++i) {
        wrapped[i].opcode =
            static_cast<std::uint16_t>(harken::kOpcodeSetVoiceVolume + i);
    }
    ASSERT_TRUE(queue.TryPushBatch(wrapped, 3));

    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(harken::kOpcodeSetBusVolume, out.opcode);
    EXPECT_EQ(2u, out.sequence);
    EXPECT_EQ(harken::kCommandVersion, out.version);
    for (std::uint16_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(queue.TryPop(out));
        EXPECT_EQ(static_cast<std::uint16_t>(
                      harken::kOpcodeSetVoiceVolume + i),
                  out.opcode);
        EXPECT_EQ(static_cast<std::uint32_t>(3 + i), out.sequence);
        EXPECT_EQ(harken::kCommandVersion, out.version);
    }
    EXPECT_FALSE(queue.TryPop(out));
}

TEST(Transport, CommandQueueReservationStaysInvisibleUntilCommit) {
    harken::CommandQueue queue(4);
    harken::CommandQueue::BatchReservation reservation{};
    ASSERT_TRUE(queue.ReserveBatch(2, &reservation));

    EXPECT_EQ(4u, queue.FreeSlots());
    EXPECT_EQ(0u, queue.OpcodeSeenMask());
    harken::CommandRecord out{};
    EXPECT_FALSE(queue.TryPop(out));

    harken::CommandRecord records[2]{};
    records[0].opcode = harken::kOpcodePlay;
    records[1].opcode = harken::kOpcodeStop;
    ASSERT_TRUE(queue.CommitReservedBatch(&reservation, records));

    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(harken::kOpcodePlay, out.opcode);
    EXPECT_EQ(0u, out.sequence);
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(harken::kOpcodeStop, out.opcode);
    EXPECT_EQ(1u, out.sequence);
    EXPECT_FALSE(queue.TryPop(out));
}

TEST(Transport, CommandQueueReservationCancelHasNoObservableSideEffects) {
    harken::CommandQueue queue(4);
    harken::CommandQueue::BatchReservation reservation{};
    ASSERT_TRUE(queue.ReserveBatch(3, &reservation));
    queue.CancelReservedBatch(&reservation);

    EXPECT_EQ(4u, queue.FreeSlots());
    EXPECT_EQ(0u, queue.OpcodeSeenMask());
    EXPECT_EQ(0u, queue.BackpressureCount());
    harken::CommandRecord out{};
    EXPECT_FALSE(queue.TryPop(out));

    harken::CommandRecord record{};
    record.opcode = harken::kOpcodePlay;
    ASSERT_TRUE(queue.TryPush(record));
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(0u, out.sequence);
}

TEST(Transport, CommandQueueReservationExcludesOtherProducerPublication) {
    harken::CommandQueue queue(4);
    harken::CommandRecord existing{};
    existing.opcode = harken::kOpcodePlay;
    ASSERT_TRUE(queue.TryPush(existing));

    harken::CommandQueue::BatchReservation reservation{};
    ASSERT_TRUE(queue.ReserveBatch(2, &reservation));
    EXPECT_EQ(3u, queue.FreeSlots());

    harken::CommandRecord rejected{};
    rejected.opcode = harken::kOpcodeSetBusVolume;
    EXPECT_FALSE(queue.TryPush(rejected));
    EXPECT_FALSE(queue.TryPushBatch(&rejected, 1));
    EXPECT_FALSE(queue.TryPushBatch(nullptr, 0));
    harken::CommandQueue::BatchReservation second{};
    EXPECT_FALSE(queue.ReserveBatch(1, &second));
    EXPECT_EQ(0u, queue.BackpressureCount());

    // The consumer may drain already-published records while the producer
    // ticket remains live. That only increases the physical free space.
    harken::CommandRecord out{};
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(harken::kOpcodePlay, out.opcode);
    EXPECT_EQ(0u, out.sequence);
    EXPECT_EQ(4u, queue.FreeSlots());

    harken::CommandRecord committed[2]{};
    committed[0].opcode = harken::kOpcodeStop;
    committed[1].opcode = harken::kOpcodeSetBusVolume;
    ASSERT_TRUE(queue.CommitReservedBatch(&reservation, committed));
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(harken::kOpcodeStop, out.opcode);
    EXPECT_EQ(1u, out.sequence);
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(harken::kOpcodeSetBusVolume, out.opcode);
    EXPECT_EQ(2u, out.sequence);
    EXPECT_FALSE(queue.TryPop(out));
}

TEST(Transport, CommandQueueReservationHandlesEmptyNullAndCapacityEdges) {
    harken::CommandQueue queue(4);
    EXPECT_TRUE(queue.ReserveBatch(0, nullptr));
    EXPECT_FALSE(queue.ReserveBatch(1, nullptr));
    EXPECT_FALSE(queue.CommitReservedBatch(nullptr, nullptr));
    queue.CancelReservedBatch(nullptr);
    EXPECT_EQ(4u, queue.FreeSlots());
    EXPECT_EQ(0u, queue.BackpressureCount());

    harken::CommandQueue::BatchReservation tooLarge{};
    EXPECT_FALSE(queue.ReserveBatch(5, &tooLarge));
    EXPECT_EQ(1u, queue.BackpressureCount());
    EXPECT_EQ(4u, queue.FreeSlots());

    harken::CommandQueue::BatchReservation reservation{};
    ASSERT_TRUE(queue.ReserveBatch(4, &reservation));
    EXPECT_FALSE(queue.CommitReservedBatch(&reservation, nullptr));
    harken::CommandRecord rejected{};
    rejected.opcode = harken::kOpcodePlay;
    EXPECT_FALSE(queue.TryPush(rejected));
    queue.CancelReservedBatch(&reservation);
    EXPECT_TRUE(queue.TryPush(rejected));
}

TEST(Transport, CommandQueueReservationCommitWrapsWithConsecutiveSequence) {
    harken::CommandQueue queue(4);
    harken::CommandRecord first[3]{};
    for (std::uint16_t i = 0; i < 3; ++i) {
        first[i].opcode =
            static_cast<std::uint16_t>(harken::kOpcodePlay + i);
    }
    ASSERT_TRUE(queue.TryPushBatch(first, 3));

    harken::CommandRecord out{};
    ASSERT_TRUE(queue.TryPop(out));
    ASSERT_TRUE(queue.TryPop(out));

    harken::CommandQueue::BatchReservation reservation{};
    ASSERT_TRUE(queue.ReserveBatch(3, &reservation));
    harken::CommandRecord wrapped[3]{};
    for (std::uint16_t i = 0; i < 3; ++i) {
        wrapped[i].opcode =
            static_cast<std::uint16_t>(harken::kOpcodeSetVoiceVolume + i);
    }
    ASSERT_TRUE(queue.CommitReservedBatch(&reservation, wrapped));

    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(harken::kOpcodeSetBusVolume, out.opcode);
    EXPECT_EQ(2u, out.sequence);
    for (std::uint16_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(queue.TryPop(out));
        EXPECT_EQ(static_cast<std::uint16_t>(
                      harken::kOpcodeSetVoiceVolume + i),
                  out.opcode);
        EXPECT_EQ(static_cast<std::uint32_t>(3 + i), out.sequence);
        EXPECT_EQ(harken::kCommandVersion, out.version);
    }
    EXPECT_FALSE(queue.TryPop(out));
}

TEST(Transport, CommandQueueReservationRejectsStaleAndForeignTickets) {
    harken::CommandQueue first(2);
    harken::CommandQueue second(2);
    harken::CommandQueue::BatchReservation reservation{};
    ASSERT_TRUE(first.ReserveBatch(1, &reservation));

    harken::CommandRecord record{};
    record.opcode = harken::kOpcodePlay;
    EXPECT_FALSE(second.CommitReservedBatch(&reservation, &record));
    first.CancelReservedBatch(&reservation);
    EXPECT_FALSE(first.CommitReservedBatch(&reservation, &record));

    harken::CommandQueue::BatchReservation replacement{};
    ASSERT_TRUE(first.ReserveBatch(1, &replacement));
    ASSERT_TRUE(first.CommitReservedBatch(&replacement, &record));
    harken::CommandRecord out{};
    ASSERT_TRUE(first.TryPop(out));
    EXPECT_EQ(0u, out.sequence);
}

TEST(Transport, CommandQueueConsumerDrainsWhileReservationRemainsLive) {
    harken::CommandQueue queue(8);
    harken::CommandRecord visible[4]{};
    for (std::uint16_t i = 0; i < 4; ++i) {
        visible[i].opcode =
            static_cast<std::uint16_t>(harken::kOpcodePlay + i);
    }
    ASSERT_TRUE(queue.TryPushBatch(visible, 4));

    harken::CommandQueue::BatchReservation reservation{};
    ASSERT_TRUE(queue.ReserveBatch(4, &reservation));
    std::atomic<bool> start{false};
    std::uint32_t drained = 0;
    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        harken::CommandRecord out{};
        while (drained < 4 && queue.TryPop(out)) {
            ++drained;
        }
    });
    start.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_EQ(4u, drained);
    EXPECT_EQ(8u, queue.FreeSlots());
    harken::CommandRecord rejected{};
    rejected.opcode = harken::kOpcodePlay;
    EXPECT_FALSE(queue.TryPush(rejected));

    harken::CommandRecord committed[4]{};
    for (std::uint16_t i = 0; i < 4; ++i) {
        committed[i].opcode =
            static_cast<std::uint16_t>(harken::kOpcodeSetVoiceVolume + i);
    }
    ASSERT_TRUE(queue.CommitReservedBatch(&reservation, committed));
    harken::CommandRecord out{};
    for (std::uint16_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(queue.TryPop(out));
        EXPECT_EQ(static_cast<std::uint16_t>(
                      harken::kOpcodeSetVoiceVolume + i),
                  out.opcode);
        EXPECT_EQ(static_cast<std::uint32_t>(4 + i), out.sequence);
    }
}

TEST(Transport, NotificationQueueDropsOldestAndCounts) {
    harken::NotificationQueue queue(2);
    EXPECT_EQ(2u, queue.Capacity());

    for (std::uint64_t i = 1; i <= 4; ++i) {
        VnNotification notification{};
        notification.type = VN_NOTIFY_VOICE_ENDED;
        notification.blockTimestamp = i;
        queue.Push(notification);
    }

    // The survivor window is capacity - 1 == 1: only the newest record
    // survives. The three older ones — including the boundary slot the
    // producer's next Push would reclaim — are dropped and counted at drain
    // time.
    VnNotification out{};
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(4u, out.blockTimestamp);
    EXPECT_FALSE(queue.TryPop(out));
    EXPECT_EQ(3u, queue.DroppedCount());
}

TEST(Transport, UpdateRequiresInit) {
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_Update());
}

TEST(Transport, CommandBackpressureRefusesThenRetriesAfterDrain) {
    // Command capacity 2: one play plus one stop fill the queue.
    VnConfig config = MakeNullConfig(4, kBlockFrames);
    config.commandQueueDepth = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle first = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));           // queue: 1/2
    ASSERT_EQ(VN_OK, Vn_Stop(first, 1000));             // queue: 2/2

    // Stop refusal: full queue, no state change, counter increments.
    EXPECT_EQ(VN_BACKPRESSURE, Vn_Stop(first, 1000));
    EXPECT_EQ(1u, session.Engine()->commands.BackpressureCount());

    // Play refusal must have zero side effects: no slot claimed, the out
    // handle untouched, no voice-pool refusal counted, and no PCM
    // reference taken. The refused request targets a sound registered only
    // for it, so unregistering that sound cleanly afterwards is external
    // proof the reference count never moved (a leaked reference would
    // refuse with VN_IN_USE) — and it leaves no state behind for the rest
    // of the test.
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &desc));
    const VnPlayDesc rejectedPlay = MakePlayDesc(harkentest::ObjectId(2));
    VnVoiceHandle second = 0xABCDu;
    EXPECT_EQ(VN_BACKPRESSURE, harkentest::PlayRaw(&rejectedPlay, &second));
    EXPECT_EQ(0xABCDu, second);
    EXPECT_EQ(1u, session.Engine()->voices.ActiveCount());
    EXPECT_EQ(0u, session.Engine()->voices.NoVoiceCount());
    EXPECT_EQ(2u, session.Engine()->commands.BackpressureCount());
    EXPECT_EQ(VN_OK, Vn_UnregisterPCM(harkentest::ObjectId(2)));

    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetBusVolume(VN_BUS_MASTER, -3.0f, 0));
    EXPECT_EQ(3u, session.Engine()->commands.BackpressureCount());

    // One pump drains the queue; the identical calls now succeed.
    session.Engine()->nullBackend.Pump(1);
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    EXPECT_NE(0xABCDu, second);
    EXPECT_EQ(2u, session.Engine()->voices.ActiveCount());
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -3.0f, 0));
    EXPECT_EQ(3u, session.Engine()->commands.BackpressureCount());

    // Draining notifications with no callback configured is a benign no-op.
    EXPECT_EQ(VN_OK, Vn_Update());
}

TEST(Transport, VoiceEndedNotificationDeliveredOnlyInsideUpdate) {
    NotificationLog log;
    EngineSession session(ConfigWithLog(&log, 2));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> shortDc = harkentest::MakeDC(64, 0.5f);
    const VnPCMDesc desc = MonoDesc(shortDc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // The 64-frame sound ends inside the first block; the notification is queued
    // on the audio side but the callback must not run during the pump.
    session.Engine()->nullBackend.Pump(1);
    EXPECT_EQ(0u, session.Engine()->voices.ActiveCount());
    EXPECT_TRUE(log.notifications.empty());

    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(1u, log.notifications.size());
    EXPECT_EQ(static_cast<std::uint32_t>(VN_NOTIFY_VOICE_ENDED),
              log.notifications[0].type);
    EXPECT_EQ(handle, log.notifications[0].voiceHandle);
    EXPECT_EQ(0u, log.notifications[0].blockTimestamp);

    // A second pump-less update delivers nothing further.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, log.notifications.size());
}

TEST(Transport, FadeToZeroEmitsStoppedByFadeOnly) {
    NotificationLog log;
    EngineSession session(ConfigWithLog(&log, 2));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    // 1 ms at 48 kHz = 48 fade frames: the ramp completes inside the next
    // block, releasing the voice.
    ASSERT_EQ(VN_OK, Vn_Stop(handle, 1));
    session.Engine()->nullBackend.Pump(1);
    EXPECT_EQ(0u, session.Engine()->voices.ActiveCount());

    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(1u, log.notifications.size()); // stopped-by-fade, no second ended
    EXPECT_EQ(static_cast<std::uint32_t>(VN_NOTIFY_VOICE_STOPPED_BY_FADE),
              log.notifications[0].type);
    EXPECT_EQ(handle, log.notifications[0].voiceHandle);
    EXPECT_EQ(1u, log.notifications[0].blockTimestamp);
}

TEST(Transport, ImmediateStopEmitsVoiceEnded) {
    NotificationLog log;
    EngineSession session(ConfigWithLog(&log, 2));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    // An immediate cut has no fade ramp, so it reports as a plain ended
    // voice when the stop command applies at the next block boundary.
    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    session.Engine()->nullBackend.Pump(1);

    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(1u, log.notifications.size());
    EXPECT_EQ(static_cast<std::uint32_t>(VN_NOTIFY_VOICE_ENDED),
              log.notifications[0].type);
    EXPECT_EQ(handle, log.notifications[0].voiceHandle);
    EXPECT_EQ(1u, log.notifications[0].blockTimestamp);
}

TEST(Transport, OrdinaryVoiceArenaExhaustionStillEmitsDegradation) {
    NotificationLog log;
    VnConfig config = ConfigWithLog(&log, 2);
    // Room for the master bus accumulators (2 planes x 256 frames x 4 bytes
    // = 2048 bytes) but not for one mono voice scratch plane (1024 bytes):
    // the voice degrades to silence for the block and the arena overflow
    // raises exactly one degradation notification.
    config.arenaBytes = 2560;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);
    EXPECT_GT(session.Engine()->mixer.Arena().OverflowCount(), 0u);

    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(1u, log.notifications.size());
    EXPECT_EQ(static_cast<std::uint32_t>(VN_NOTIFY_DEGRADATION_OCCURRED),
              log.notifications[0].type);
    EXPECT_EQ(0u, log.notifications[0].voiceHandle); // not bound to one voice
    EXPECT_EQ(0u, log.notifications[0].blockTimestamp);
}

TEST(Transport, NotificationProducerBudgetAndRingOverwriteAreSeparatedThroughApi) {
    NotificationLog log;
    EngineSession session(ConfigWithLog(&log, 4, /*notificationQueueDepth=*/2));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> shortDc = harkentest::MakeDC(64, 0.5f);
    const VnPCMDesc desc = MonoDesc(shortDc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handles[4] = {};
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handles[i])) << "voice " << i;
    }

    // All four voices end in one block. The producer budget accepts the first
    // two notifications and refuses the latter two; the two accepted records
    // then enter a capacity-two ring whose survivor window is capacity - 1,
    // so the first accepted record is overwritten before the newest survives
    // the drain.
    session.Engine()->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    ASSERT_EQ(1u, log.notifications.size());
    EXPECT_EQ(handles[1], log.notifications[0].voiceHandle);
    EXPECT_EQ(1u, session.Engine()->notifications.DroppedCount());
    EXPECT_EQ(2u, session.Engine()->notifications.QuantumBudgetDropCount());
    EXPECT_EQ(2u,
              session.Engine()->notifications.QuantumProducedHighWater());
    EXPECT_EQ(4u,
              static_cast<std::uint64_t>(log.notifications.size()) +
                  session.Engine()->notifications.DroppedCount() +
                  session.Engine()->notifications.QuantumBudgetDropCount());
}

} // namespace
