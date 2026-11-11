// Built-in effect behavior, validation, pooling, and lifecycle. The two real
// effects (feedback delay, three-band tone) are driven through the public API
// so the whole path is exercised (desc validate -> pool acquire -> command ->
// apply -> render), plus a few direct-state checks of reset determinism that
// the API cannot observe. DSP correctness here is structural (an echo appears
// where computed, feedback decays, a shelf changes the DC level); precise
// closed-form outputs live in the golden cases.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "Common/EffectPoolTestAccess.h"
#include <thread>
#include <atomic>
#include "harken/dsp/Biquad.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/EffectChain.h"

#include <cmath>
#include <cstdint>
#include <vector>
#include <limits>
#include <utility>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

struct EffectExtraAllocationFault {
    EffectExtraAllocationFault() {
        harken::detail::ArmLogicAllocFaultForTests(1, harken::detail::LogicAllocSite::EffectExtraStorage);
    }
    ~EffectExtraAllocationFault() { harken::detail::DisarmLogicAllocFaultForTests(); }
};

VnConfig EffectConfig(std::uint32_t slots = 1, std::uint32_t delayCap = 0,
                      std::uint32_t toneCap = 0) {
    VnConfig config = harkentest::MakeNullConfig(0, kBlock, kRate);
    config.fxSlotsPerVoice = slots;
    config.delayEffectPoolCapacity = delayCap;
    config.toneEffectPoolCapacity = toneCap;
    return config;
}

VnFeedbackDelayDesc DelayDesc(float delayMs, float feedback, float wet,
                              float dry) {
    VnFeedbackDelayDesc d{};
    d.structSize = sizeof(VnFeedbackDelayDesc);
    d.delayMs = delayMs;
    d.feedback = feedback;
    d.wet = wet;
    d.dry = dry;
    return d;
}

VnThreeBandToneDesc ToneDesc(float lsG, float lsF, float pG, float pF,
                             float pQ, float hsG, float hsF) {
    VnThreeBandToneDesc t{};
    t.structSize = sizeof(VnThreeBandToneDesc);
    t.lowShelfGainDb = lsG;
    t.lowShelfFreqHz = lsF;
    t.peakGainDb = pG;
    t.peakFreqHz = pF;
    t.peakQ = pQ;
    t.highShelfGainDb = hsG;
    t.highShelfFreqHz = hsF;
    return t;
}

const VnEffectDesc* AsBase(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

VnVoiceHandle PlayMono(EngineSession& session, VnSoundId id,
                       const std::vector<float>& samples) {
    const VnPCMDesc desc = MonoDesc(samples, kRate);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
    const VnPlayDesc play = MakePlayDesc(id);
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    return handle;
}

// --- feedback delay behavior ---

TEST(BuiltinDelay, ProducesDelayedEchoAtComputedPosition) {
    // A single delayed copy (feedback 0) of an impulse must appear exactly at
    // delaySamples = delayMs * rate / 1000 = 10 * 48 = 480, and nowhere
    // between. Proves the delay actually delays by the configured time.
    EngineSession session(EffectConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> impulse(1200, 0.0f);
    impulse[0] = 1.0f;
    VnVoiceHandle handle = PlayMono(session, harkentest::ObjectId(1), impulse);

    const VnFeedbackDelayDesc d = DelayDesc(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&d)));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 5, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    ASSERT_GT(out.left.size(), 481u);
    EXPECT_NEAR(1.0f, out.left[0], 1e-4f);   // original, dry
    EXPECT_NEAR(1.0f, out.left[480], 1e-4f); // echo, one delay period later
    EXPECT_NEAR(0.0f, out.left[240], 1e-4f); // nothing in between
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(BuiltinDelay, FeedbackProducesDecayingRepeats) {
    // With feedback, the impulse echoes repeatedly at 480, 960, 1440, each
    // strictly smaller than the last (a decaying feedback loop). No feedback
    // would leave only the first echo.
    EngineSession session(EffectConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> impulse(2200, 0.0f);
    impulse[0] = 1.0f;
    VnVoiceHandle handle = PlayMono(session, harkentest::ObjectId(1), impulse);

    const VnFeedbackDelayDesc d = DelayDesc(10.0f, 0.5f, 1.0f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&d)));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 8, out);
    ASSERT_GT(out.left.size(), 1441u);
    const float e1 = out.left[480];
    const float e2 = out.left[960];
    const float e3 = out.left[1440];
    EXPECT_NEAR(1.0f, e1, 1e-4f); // first echo = wet * impulse
    EXPECT_GT(e1, e2);
    EXPECT_GT(e2, e3);
    EXPECT_GT(e3, 0.0f);
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(BuiltinDelay, TailReportMatchesMinus60dbFormula) {
    // The reported tail must follow the -60 dB decay formula,
    // quantitatively — not just the max-over-slots relation the
    // aggregate-tail golden checks. With no feedback the tail is a single delay
    // period; with feedback fb in (0, 1) it is delay * (n + 1) where
    // n = ceil(ln(1e-3)/ln(fb)) is the loop-period count that brings the echo
    // below -60 dB (the extra period covers the initial delay). The oracle is
    // computed here straight from that formula, INDEPENDENT of the production
    // DelayTail body, and matched exactly (integer, tolerance 0) — so
    // short-circuiting the feedback branch to report only one delay period
    // (ignoring feedback) is caught. delaySamples is read back from the
    // configured instance so the comparison isolates the tail formula, not the
    // time-to-samples conversion.
    std::vector<float> line(static_cast<std::size_t>(harken::kMaxChannels) * 4096,
                            0.0f);

    auto check = [&](float delayMs, float feedback) {
        harken::DelayEffectState st;
        st.lineBase = line.data();
        st.lineFrames = 4096;
        st.sampleRate = kRate;
        harken::InsertEffect fx =
            harken::BuildEffect(VN_EFFECT_FEEDBACK_DELAY, &st);
        ASSERT_NE(nullptr, fx.setParam);
        ASSERT_NE(nullptr, fx.tailFrames);
        fx.setParam(&st, harken::kDelayParamTimeMs, delayMs);
        fx.setParam(&st, harken::kDelayParamFeedback, feedback);

        const std::uint32_t delay = st.delaySamples;
        std::uint32_t expected;
        if (!(feedback > 0.0f)) {
            expected = delay; // one delayed copy, one delay period of tail
        } else {
            const double n = std::ceil(std::log(1.0e-3) /
                                       std::log(static_cast<double>(feedback)));
            expected = static_cast<std::uint32_t>(static_cast<double>(delay) *
                                                  (n + 1.0));
        }
        EXPECT_EQ(expected, fx.tailFrames(&st))
            << "delayMs=" << delayMs << " feedback=" << feedback
            << " delaySamples=" << delay;
    };

    check(10.0f, 0.0f); // no feedback: tail == delay
    check(10.0f, 0.5f); // n = ceil(ln(1e-3)/ln(0.5)) = 10 -> tail = delay * 11
    check(25.0f, 0.9f); // n = ceil(ln(1e-3)/ln(0.9)) = 66 -> tail = delay * 67
}

// --- three-band tone behavior ---

TEST(BuiltinTone, LowShelfChangesDcGainByComputedFactor) {
    // A +12 dB low shelf lifts DC by the section's DC gain; the peak and high
    // shelf sit at 0 dB (exactly transparent). The steady-state output equals
    // the input times the product of the three sections' DC gains, computed
    // here from the same cookbook coefficients the effect uses.
    EngineSession session(EffectConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());
    constexpr float kDc = 0.2f;
    std::vector<float> dc(4000, kDc);
    VnVoiceHandle handle = PlayMono(session, harkentest::ObjectId(1), dc);

    const VnThreeBandToneDesc t =
        ToneDesc(12.0f, 200.0f, 0.0f, 1000.0f, 1.0f, 0.0f, 6000.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_THREE_BAND_TONE,
                                       AsBase(&t)));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 8, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    auto dcGain = [](const harken::BiquadCoeffs& c) {
        return (c.b0 + c.b1 + c.b2) / (1.0f + c.a1 + c.a2);
    };
    const float g =
        dcGain(harken::MakeLowShelf(static_cast<float>(kRate), 200.0f,
                                   harken::kToneShelfQ, 12.0f)) *
        dcGain(harken::MakePeaking(static_cast<float>(kRate), 1000.0f, 1.0f,
                                  0.0f)) *
        dcGain(harken::MakeHighShelf(static_cast<float>(kRate), 6000.0f,
                                    harken::kToneShelfQ, 0.0f));
    const float expected = kDc * g;
    EXPECT_GT(g, 1.5f); // a real boost, not a no-op
    EXPECT_NEAR(expected, out.left.back(), 1e-3f);
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

// --- descriptor and parameter validation ---

TEST(BuiltinEffectValidation, RejectsOutOfRangeAndMalformedDescriptors) {
    EngineSession session(EffectConfig(2));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(400, 1000.0, kRate, 0.5f);
    VnVoiceHandle handle = PlayMono(session, harkentest::ObjectId(1), sine);

    // Feedback at/above unity, delay out of range, non-finite: all refused.
    VnFeedbackDelayDesc d = DelayDesc(10.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_FEEDBACK_DELAY, AsBase(&d)));
    d = DelayDesc(5000.0f, 0.5f, 1.0f, 1.0f); // > 1000 ms max
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_FEEDBACK_DELAY, AsBase(&d)));
    d = DelayDesc(0.0f, 0.5f, 1.0f, 1.0f); // non-positive delay
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_FEEDBACK_DELAY, AsBase(&d)));

    // Tone frequency at/above Nyquist and non-positive Q refused.
    VnThreeBandToneDesc t =
        ToneDesc(0.0f, 200.0f, 6.0f, 30000.0f, 1.0f, 0.0f, 6000.0f);
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceEffect(
                                  handle, 0, VN_EFFECT_THREE_BAND_TONE,
                                  AsBase(&t)));
    t = ToneDesc(0.0f, 200.0f, 6.0f, 1000.0f, 0.0f, 0.0f, 6000.0f);
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceEffect(
                                  handle, 0, VN_EFFECT_THREE_BAND_TONE,
                                  AsBase(&t)));

    // Unknown type, wrong struct size, null desc, out-of-range slot.
    VnFeedbackDelayDesc good = DelayDesc(10.0f, 0.5f, 1.0f, 1.0f);
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffect(handle, 0, 99u, AsBase(&good)));
    VnFeedbackDelayDesc badSize = good;
    badSize.structSize = sizeof(VnFeedbackDelayDesc) + 4;
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceEffect(
                                  handle, 0, VN_EFFECT_FEEDBACK_DELAY,
                                  AsBase(&badSize)));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceEffect(
                                  handle, 0, VN_EFFECT_FEEDBACK_DELAY, nullptr));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceEffect(
                                  handle, 2, VN_EFFECT_FEEDBACK_DELAY,
                                  AsBase(&good))); // slot >= fxSlotsPerVoice
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(BuiltinEffectValidation, ParamUpdateRangeIsTypeSpecific) {
    EngineSession session(EffectConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(400, 1000.0, kRate, 0.5f);
    VnVoiceHandle handle = PlayMono(session, harkentest::ObjectId(1), sine);

    // No effect installed yet: a param has no type to validate against.
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffectParam(handle, 0, harken::kDelayParamFeedback,
                                     0.5f));

    const VnFeedbackDelayDesc d = DelayDesc(10.0f, 0.5f, 1.0f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&d)));

    // In range accepted; feedback >= 1, non-finite, and unknown index refused.
    EXPECT_EQ(VN_OK, Vn_SetVoiceEffectParam(handle, 0,
                                            harken::kDelayParamFeedback, 0.9f));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffectParam(handle, 0, harken::kDelayParamFeedback,
                                     1.0f));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffectParam(handle, 0, harken::kDelayParamTimeMs,
                                     std::nanf("")));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffectParam(handle, 0, 99u, 0.5f));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(BuiltinEffectValidation, StaleHandleIsBenignAcrossEffectCalls) {
    EngineSession session(EffectConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(400, 1000.0, kRate, 0.5f);
    VnVoiceHandle handle = PlayMono(session, harkentest::ObjectId(1), sine);
    const VnFeedbackDelayDesc d = DelayDesc(10.0f, 0.5f, 1.0f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&d)));

    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    Capture drain;
    harkentest::PumpAndCapture(session.Engine(), 1, drain);
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetVoiceEffect(
                                   handle, 0, VN_EFFECT_FEEDBACK_DELAY,
                                   AsBase(&d)));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_ClearVoiceEffect(handle, 0));
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_SetVoiceEffectParam(handle, 0, harken::kDelayParamWet, 0.5f));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

// --- pooling and lifecycle ---

TEST(BuiltinEffectPool, AutomaticGrowthPreservesPublishedRowsProofsAndUndrainedRetirements) {
    harken::EffectPoolBank bank(harken::EngineConfig{});
    const auto anchor = bank.Acquire(VN_EFFECT_GAIN);
    ASSERT_EQ(VN_EFFECT_GAIN, anchor.type);
    void* const address = bank.LiveStateFor(anchor);
    const auto prepared = bank.AcquirePrepared(VN_EFFECT_GAIN);
    harken::EffectPoolBank::PreparedCommitProof proof;
    ASSERT_TRUE(bank.ProvePreparedCommit(prepared, {}, &proof));
    std::atomic<bool> done{false}, invalid{false};
    std::atomic<unsigned> reads{0};
    std::thread reader([&] {
        harken::memguard::AudioThreadScope audio;
        while (!done.load(std::memory_order_acquire)) {
            if (bank.LiveStateFor(anchor) != address) invalid.store(true);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (reads.load(std::memory_order_relaxed) == 0) std::this_thread::yield();
    std::vector<harken::EffectInstanceRef> refs;
    refs.reserve(4097);
    for (unsigned i = 0; i < 4097; ++i) {
        const auto ref = bank.Acquire(VN_EFFECT_GAIN);
        if (ref.type != VN_EFFECT_GAIN) break;
        refs.push_back(ref);
    }
    done.store(true, std::memory_order_release);
    reader.join();
    ASSERT_EQ(4097u, refs.size());
    EXPECT_FALSE(invalid.load());
    EXPECT_EQ(address, bank.LiveStateFor(anchor));
    harken::EffectInstanceRef original{};
    ASSERT_TRUE(bank.CaptureOriginalPreparedEffect(proof, &original));
    EXPECT_EQ(prepared.poolIndex, original.poolIndex);
    EXPECT_EQ(prepared.generation, original.generation);
    proof.Release();
    ASSERT_TRUE(bank.CancelPrepared(prepared));
    {
        harken::memguard::AudioThreadScope audio;
        EXPECT_TRUE(bank.Retire(anchor));
        for (const auto& ref : refs) EXPECT_TRUE(bank.Retire(ref));
        for (const auto& ref : refs) EXPECT_TRUE(bank.Retire(ref));
    }
    EXPECT_EQ(4098u, bank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(4098u, bank.DrainRetire());
    EXPECT_EQ(0u, bank.DrainRetire());
    EXPECT_EQ(0u, bank.InUseOf(VN_EFFECT_GAIN));
}

TEST(BuiltinEffectPool, RetiredGenerationSurvivesConcurrentLogicReuse) {
    harken::EngineConfig config;
    config.gainEffectPoolCapacity = 1;
    harken::EffectPoolBank bank(config);
    const auto old = bank.Acquire(VN_EFFECT_GAIN);
    ASSERT_EQ(VN_EFFECT_GAIN, old.type);
    harken::EffectInstanceRef fresh{};
    std::atomic<unsigned> phase{0};
    std::atomic<bool> valid{true};
    std::thread audio([&] {
        harken::memguard::AudioThreadScope scope;
        if (!bank.Retire(old)) valid.store(false);
        phase.store(1, std::memory_order_release);
        while (phase.load(std::memory_order_acquire) != 2) std::this_thread::yield();
        for (unsigned i = 0; i < 1000; ++i)
            if (!bank.Retire(old) || bank.LiveStateFor(fresh) == nullptr) valid.store(false);
        if (!bank.Retire(fresh)) valid.store(false);
    });
    while (phase.load(std::memory_order_acquire) != 1) std::this_thread::yield();
    EXPECT_EQ(1u, bank.DrainRetire());
    fresh = bank.Acquire(VN_EFFECT_GAIN);
    phase.store(2, std::memory_order_release);
    audio.join();
    EXPECT_EQ(old.poolIndex, fresh.poolIndex);
    EXPECT_GT(fresh.generation, old.generation);
    EXPECT_TRUE(valid.load());
    EXPECT_EQ(1u, bank.DrainRetire());
    EXPECT_EQ(0u, bank.InUseOf(VN_EFFECT_GAIN));
}

TEST(BuiltinEffectPool, ExplicitBackingSaturatesWithoutReservingLogicalIdentities) {
    harken::EngineConfig config;
    config.gainEffectPoolCapacity = 2;
    harken::EffectPoolBank bank(config);
    const auto live = bank.Acquire(VN_EFFECT_GAIN);
    ASSERT_EQ(VN_EFFECT_GAIN, live.type);
    harken::EffectPoolBank::BackingDemand demand{};
    demand[VN_EFFECT_GAIN] = 100;
    harken::EffectPoolBank::BackingPreparation first, second;
    ASSERT_TRUE(bank.PrepareBacking(demand, &first));
    ASSERT_TRUE(bank.PrepareBacking(demand, &second));
    harken::detail::ArmLogicAllocFaultForTests(1, harken::detail::LogicAllocSite::None);
    {
        auto use = harken::EffectPoolBank::UseBacking(&second);
        const auto available = bank.AcquirePrepared(VN_EFFECT_GAIN);
        EXPECT_EQ(VN_EFFECT_GAIN, available.type);
        EXPECT_EQ(harken::kEffectTypeNone, bank.AcquirePrepared(VN_EFFECT_GAIN).type);
        EXPECT_TRUE(bank.CancelPrepared(available));
        const auto retry = bank.AcquirePrepared(VN_EFFECT_GAIN);
        EXPECT_EQ(available.poolIndex, retry.poolIndex);
        EXPECT_TRUE(bank.CancelPrepared(retry));
        harken::EffectPoolBank::BackingPreparation nested;
        EXPECT_FALSE(bank.PrepareBacking(demand, &nested));
    }
    EXPECT_EQ(1u, harken::detail::g_logicAllocFault.remaining);
    harken::detail::DisarmLogicAllocFaultForTests();
    bank.Recycle(live);
}

TEST(BuiltinEffectPool, AutomaticHoldsPrepareEveryTypeAndPreserveOtherHolds) {
    harken::EffectPoolBank bank(harken::EngineConfig{});
    harken::EffectPoolBank::BackingDemand demand{};
    demand.fill(1);
    harken::EffectPoolBank::BackingPreparation first, second;
    ASSERT_TRUE(bank.PrepareBacking(demand, &first));
    ASSERT_TRUE(bank.PrepareBacking(demand, &second));
    const auto ordinary = bank.Acquire(VN_EFFECT_GAIN);
    ASSERT_EQ(VN_EFFECT_GAIN, ordinary.type);
    EXPECT_EQ(3u, bank.CapacityOf(VN_EFFECT_GAIN));
    harken::detail::ArmLogicAllocFaultForTests(1, harken::detail::LogicAllocSite::None);
    std::array<harken::EffectInstanceRef, harken::kEffectRegistryRowCount> a{}, b{};
    {
        auto use = harken::EffectPoolBank::UseBacking(&first);
        for (const auto& registry : harken::kEffectRegistry) {
            a[registry.effectType] = bank.AcquirePrepared(registry.effectType);
            EXPECT_EQ(registry.effectType, a[registry.effectType].type);
        }
    }
    {
        auto use = harken::EffectPoolBank::UseBacking(&second);
        for (const auto& registry : harken::kEffectRegistry) {
            b[registry.effectType] = bank.AcquirePrepared(registry.effectType);
            EXPECT_EQ(registry.effectType, b[registry.effectType].type);
        }
    }
    EXPECT_EQ(1u, harken::detail::g_logicAllocFault.remaining);
    harken::detail::DisarmLogicAllocFaultForTests();
    for (const auto& ref : a) EXPECT_TRUE(bank.CancelPrepared(ref));
    for (const auto& ref : b) EXPECT_TRUE(bank.CancelPrepared(ref));
    bank.Recycle(ordinary);
}

TEST(BuiltinEffectPool, FailedBackingNeverIssuesIdentityAndRemainsRetryable) {
    for (auto site : {harken::detail::LogicAllocSite::EffectInstanceStorage,
                      harken::detail::LogicAllocSite::EffectRetireStorage,
                      harken::detail::LogicAllocSite::EffectExtraStorage}) {
        harken::EffectPoolBank bank(harken::EngineConfig{});
        harken::detail::ArmLogicAllocFaultForTests(1, site);
        EXPECT_EQ(harken::kEffectTypeNone, bank.AcquirePrepared(VN_EFFECT_FEEDBACK_DELAY).type);
        EXPECT_EQ(0u, bank.InUseOf(VN_EFFECT_FEEDBACK_DELAY));
        EXPECT_EQ(0u, bank.HighWaterOf(VN_EFFECT_FEEDBACK_DELAY));
        harken::detail::DisarmLogicAllocFaultForTests();
        const auto retry = bank.AcquirePrepared(VN_EFFECT_FEEDBACK_DELAY);
        ASSERT_EQ(VN_EFFECT_FEEDBACK_DELAY, retry.type);
        EXPECT_EQ(1u, retry.generation);
        EXPECT_TRUE(bank.CancelPrepared(retry));
    }
}

TEST(BuiltinEffectPool, PreparedCommitHeldBlocksMutationAndAbortRestoresOriginalState) {
    auto config = EffectConfig();
    config.gainEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& bank = session.Engine()->effectBank;
    const harken::EffectObjectIdentity identity{harkentest::ObjectId(810), harkentest::ObjectId(811)};
    const auto ref = bank.AcquirePrepared(VN_EFFECT_GAIN, identity);
    ASSERT_EQ(VN_EFFECT_GAIN, ref.type);
    auto* state = static_cast<harken::GainState*>(bank.StateFor(ref.type, ref.poolIndex));
    ASSERT_NE(nullptr, state);
    state->gainLinear = state->targetGainLinear = 0.5f;
    harken::EffectPoolBank::PreparedCommitProof proof;
    ASSERT_TRUE(bank.ProvePreparedCommit(ref, identity, &proof));
    EXPECT_FALSE(bank.CommitPrepared(ref));
    EXPECT_FALSE(bank.CancelPrepared(ref));
    bank.Recycle(ref);
    bank.PublishInstalled(ref);
    ASSERT_TRUE(bank.Retire(ref));
    bank.DrainRetire();
    EXPECT_EQ(nullptr, bank.StateFor(ref.type, ref.poolIndex));
    EXPECT_EQ(nullptr, bank.LiveStateFor(ref));
    EXPECT_EQ(1u, bank.InUseOf(ref.type));
    EXPECT_EQ(0u, bank.HighWaterOf(ref.type));
    EXPECT_EQ(identity.effectId, bank.ObjectIdentity(ref).effectId);
    auto moved = std::move(proof);
    EXPECT_FALSE(proof.Valid());
    ASSERT_TRUE(moved.Valid());
    moved.Release();
    EXPECT_EQ(state, bank.StateFor(ref.type, ref.poolIndex));
    EXPECT_FLOAT_EQ(0.5f, state->gainLinear);
    EXPECT_FLOAT_EQ(0.5f, state->targetGainLinear);
    EXPECT_TRUE(bank.CancelPrepared(ref));
    bank.Recycle(ref);
    EXPECT_EQ(0u, bank.InUseOf(ref.type));
    EXPECT_EQ(nullptr, bank.StateFor(ref.type, ref.poolIndex));
    EXPECT_EQ(VnObjectId{}, bank.ObjectIdentity(ref).effectId);
}

TEST(BuiltinEffectPool, PreparedCommitOriginalIdentityRejectsReuseAndMaximumNeverWraps) {
    auto config = EffectConfig();
    config.gainEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& bank = session.Engine()->effectBank;
    const harken::EffectObjectIdentity identity{harkentest::ObjectId(812), harkentest::ObjectId(813)};
    const auto old = bank.AcquirePrepared(VN_EFFECT_GAIN, identity);
    ASSERT_TRUE(bank.CancelPrepared(old));
    const auto fresh = bank.AcquirePrepared(VN_EFFECT_GAIN, identity);
    ASSERT_EQ(old.poolIndex, fresh.poolIndex);
    EXPECT_GT(fresh.generation, old.generation);
    harken::EffectPoolBank::PreparedCommitProof proof;
    EXPECT_FALSE(bank.ProvePreparedCommit(old, identity, &proof));
    EXPECT_FALSE(bank.CancelPrepared(old));
    bank.Recycle(old);
    EXPECT_EQ(1u, bank.InUseOf(fresh.type));
    ASSERT_TRUE(bank.ProvePreparedCommit(fresh, identity, &proof));
    proof.Release();
    ASSERT_TRUE(bank.CancelPrepared(fresh));
    ASSERT_TRUE(harken::detail::EffectPoolTestAccess::SeedFreeGeneration(
        bank, fresh, std::numeric_limits<std::uint64_t>::max() - 1u));
    const auto last = bank.AcquirePrepared(VN_EFFECT_GAIN, identity);
    ASSERT_EQ(std::numeric_limits<std::uint64_t>::max(), last.generation);
    ASSERT_TRUE(bank.ProvePreparedCommit(last, identity, &proof));
    proof.Release();
    ASSERT_TRUE(bank.CancelPrepared(last));
    bank.Recycle(last);
    EXPECT_EQ(0u, bank.InUseOf(last.type));
    EXPECT_EQ(harken::kEffectTypeNone, bank.AcquirePrepared(VN_EFFECT_GAIN, identity).type);
    EXPECT_EQ(harken::kEffectTypeNone, bank.Acquire(VN_EFFECT_GAIN, identity).type);
}

TEST(BuiltinEffectPool, PreparedCommitPrefixRefusalMoveAssignmentAndUniqueConsumeConserveRights) {
    auto config = EffectConfig();
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& bank = session.Engine()->effectBank;
    const harken::EffectObjectIdentity identity{harkentest::ObjectId(814), harkentest::ObjectId(815)};
    const auto first = bank.AcquirePrepared(VN_EFFECT_GAIN, identity);
    const auto second = bank.AcquirePrepared(VN_EFFECT_GAIN, identity);
    harken::EffectPoolBank::PreparedCommitProof a, b;
    ASSERT_TRUE(bank.ProvePreparedCommit(first, identity, &a));
    auto bad = identity;
    bad.projectId = harkentest::ObjectId(816);
    EXPECT_FALSE(bank.ProvePreparedCommit(second, bad, &b));
    EXPECT_FALSE(b.Valid());
    EXPECT_NE(nullptr, bank.StateFor(second.type, second.poolIndex));
    ASSERT_TRUE(bank.ProvePreparedCommit(second, identity, &b));
    a = std::move(b);
    EXPECT_FALSE(b.Valid());
    ASSERT_TRUE(bank.CancelPrepared(first));
    EXPECT_FALSE(bank.CancelPrepared(second));
    std::move(a).Consume();
    EXPECT_FALSE(a.Valid());
    EXPECT_NE(nullptr, bank.LiveStateFor(second));
    EXPECT_FALSE(bank.CommitPrepared(second));
    EXPECT_EQ(1u, bank.HighWaterOf(second.type));
    bank.Recycle(second);
    bank.Recycle(second);
    EXPECT_EQ(0u, bank.InUseOf(second.type));
    const auto one = bank.Acquire(VN_EFFECT_GAIN);
    const auto two = bank.Acquire(VN_EFFECT_GAIN);
    EXPECT_NE(one.poolIndex, two.poolIndex);
    EXPECT_EQ(harken::kEffectTypeNone, bank.Acquire(VN_EFFECT_GAIN).type);
    bank.Recycle(one);
    bank.Recycle(two);
}

TEST(BuiltinEffectPool, VoiceAndBusHostsRejectPreparedHeldAndStaleEffectBeforeReplacingLiveSlot) {
    auto config = EffectConfig();
    config.gainEffectPoolCapacity = 3;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    std::vector<float> pcm(4096, 0.25f);
    const auto handle = PlayMono(session, harkentest::ObjectId(817), pcm);
    engine->nullBackend.Pump(1);
    const auto render = harkentest::RenderHandle(engine, handle);
    ASSERT_NE(nullptr, engine->voices.Resolve(render));
    auto& bank = engine->effectBank;
    const auto voiceFx = bank.Acquire(VN_EFFECT_GAIN);
    const auto busFx = bank.Acquire(VN_EFFECT_GAIN);
    const auto incoming = bank.AcquirePrepared(VN_EFFECT_GAIN);
    auto* voiceGain = static_cast<harken::GainState*>(bank.StateFor(voiceFx.type, voiceFx.poolIndex));
    auto* busGain = static_cast<harken::GainState*>(bank.StateFor(busFx.type, busFx.poolIndex));
    ASSERT_NE(nullptr, voiceGain);
    ASSERT_NE(nullptr, busGain);
    voiceGain->gainLinear = voiceGain->targetGainLinear = 0.5f;
    busGain->gainLinear = busGain->targetGainLinear = 0.5f;
    const auto master = engine->buses.FindRef(VN_BUS_MASTER);
    ASSERT_TRUE(master.Valid());
    engine->voices.ApplySetVoiceEffect(render, 0, voiceFx.type, voiceFx.poolIndex, voiceFx.generation);
    engine->buses.ApplySetBusEffect(master.index, 0, busFx.type, busFx.poolIndex, busFx.generation, master.generation);
    auto checkRejected = [&](const harken::EffectInstanceRef& rejected) {
        engine->voices.ApplySetVoiceEffect(render, 0, rejected.type, rejected.poolIndex, rejected.generation);
        engine->buses.ApplySetBusEffect(master.index, 0, rejected.type, rejected.poolIndex, rejected.generation, master.generation);
        const auto* voice = engine->voices.Resolve(render);
        const auto* bus = engine->buses.BusState(master.index);
        ASSERT_NE(nullptr, voice);
        ASSERT_NE(nullptr, bus);
        EXPECT_EQ(voiceFx.generation, voice->fxSlotEffectRef[0].generation);
        EXPECT_EQ(voiceFx.poolIndex, voice->fxSlotEffectRef[0].poolIndex);
        EXPECT_EQ(busFx.generation, bus->fxSlotEffectRef[0].generation);
        EXPECT_EQ(busFx.poolIndex, bus->fxSlotEffectRef[0].poolIndex);
        bank.DrainRetire();
        EXPECT_EQ(3u, bank.InUseOf(VN_EFFECT_GAIN));
        engine->nullBackend.Pump(1);
        const auto* output = engine->nullBackend.LastBlock();
        ASSERT_NE(nullptr, output);
        for (std::uint32_t i = 0; i < engine->nullBackend.BlockFrames() * engine->nullBackend.Channels(); ++i)
            EXPECT_NEAR(0.0625f, output[i], 1e-5f);
    };
    ASSERT_NO_FATAL_FAILURE(checkRejected(incoming));
    harken::EffectPoolBank::PreparedCommitProof proof;
    ASSERT_TRUE(bank.ProvePreparedCommit(incoming, {}, &proof));
    ASSERT_NO_FATAL_FAILURE(checkRejected(incoming));
    engine->voices.ApplySetVoiceEffect(0, 0, incoming.type, incoming.poolIndex, incoming.generation);
    bank.DrainRetire();
    EXPECT_TRUE(proof.Valid());
    proof.Release();
    ASSERT_TRUE(bank.CancelPrepared(incoming));
    const auto fresh = bank.AcquirePrepared(VN_EFFECT_GAIN);
    ASSERT_GT(fresh.generation, incoming.generation);
    ASSERT_NO_FATAL_FAILURE(checkRejected(incoming));
    ASSERT_TRUE(bank.CancelPrepared(fresh));
    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    engine->buses.ApplyClearBusEffect(master.index, 0, master.generation);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, bank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(BuiltinEffectPool, ExhaustionRefusesWithNoEffect) {
    // One delay instance in the pool; the tone pool is independent.
    EngineSession session(EffectConfig(1, /*delayCap=*/1, /*toneCap=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(400, 1000.0, kRate, 0.5f);

    const VnPCMDesc pcm = MonoDesc(sine, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle a = 0, b = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &b));

    const VnFeedbackDelayDesc d = DelayDesc(10.0f, 0.5f, 1.0f, 1.0f);
    const auto publishedBefore = session.Engine()->commands.PublishedHead();
    {
        EffectExtraAllocationFault fault;
        EXPECT_EQ(VN_NO_EFFECT, Vn_SetVoiceEffect(a, 0, VN_EFFECT_FEEDBACK_DELAY, AsBase(&d)));
    }
    EXPECT_EQ(publishedBefore, session.Engine()->commands.PublishedHead());
    EXPECT_EQ(0u, session.Engine()->effectBank.InUseOf(VN_EFFECT_FEEDBACK_DELAY));
    EXPECT_EQ(0u, session.Engine()->effectBank.HighWaterOf(VN_EFFECT_FEEDBACK_DELAY));
    EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(a, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&d)));
    // Second delay exhausts the one-instance pool.
    EXPECT_EQ(VN_NO_EFFECT, Vn_SetVoiceEffect(b, 0, VN_EFFECT_FEEDBACK_DELAY,
                                              AsBase(&d)));
    // The tone pool is separate and still has an instance.
    const VnThreeBandToneDesc t =
        ToneDesc(6.0f, 200.0f, 0.0f, 1000.0f, 1.0f, 0.0f, 6000.0f);
    EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(b, 0, VN_EFFECT_THREE_BAND_TONE,
                                       AsBase(&t)));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(BuiltinEffectPool, AcceptedExtraBackingSurvivesOrdinaryRefusalAndPreparedRetry) {
    EngineSession session(EffectConfig(1, 2, 1));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& bank = session.Engine()->effectBank;
    harken::EffectPoolBank::BackingDemand demand{};
    demand[VN_EFFECT_FEEDBACK_DELAY] = 1;
    harken::EffectPoolBank::BackingPreparation backing;
    ASSERT_TRUE(bank.PrepareBacking(demand, &backing));
    {
        EffectExtraAllocationFault fault;
        EXPECT_EQ(harken::kEffectTypeNone, bank.AcquirePrepared(VN_EFFECT_FEEDBACK_DELAY).type);
    }
    EXPECT_EQ(0u, bank.InUseOf(VN_EFFECT_FEEDBACK_DELAY));
    EXPECT_EQ(0u, bank.HighWaterOf(VN_EFFECT_FEEDBACK_DELAY));
    {
        EffectExtraAllocationFault fault;
        auto use = harken::EffectPoolBank::UseBacking(&backing);
        const auto original = bank.AcquirePrepared(VN_EFFECT_FEEDBACK_DELAY);
        ASSERT_EQ(VN_EFFECT_FEEDBACK_DELAY, original.type);
        auto* state = static_cast<harken::DelayEffectState*>(bank.StateFor(original.type, original.poolIndex));
        ASSERT_NE(nullptr, state);
        ASSERT_NE(nullptr, state->lineBase);
        EXPECT_NE(0u, state->lineFrames);
        const auto* line = state->lineBase;
        ASSERT_TRUE(bank.CancelPrepared(original));
        const auto retry = bank.AcquirePrepared(VN_EFFECT_FEEDBACK_DELAY);
        ASSERT_EQ(VN_EFFECT_FEEDBACK_DELAY, retry.type);
        EXPECT_EQ(original.poolIndex, retry.poolIndex);
        EXPECT_EQ(original.generation + 1u, retry.generation);
        EXPECT_EQ(state, bank.StateFor(retry.type, retry.poolIndex));
        EXPECT_EQ(line, static_cast<harken::DelayEffectState*>(bank.StateFor(retry.type, retry.poolIndex))->lineBase);
        EXPECT_EQ(1u, harken::detail::g_logicAllocFault.remaining);
        EXPECT_TRUE(bank.CancelPrepared(retry));
    }
    EXPECT_EQ(0u, bank.InUseOf(VN_EFFECT_FEEDBACK_DELAY));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(BuiltinEffectPool, ClearRecyclesInstanceForReuse) {
    // With one instance, clearing it must return it to the pool so a later Set
    // succeeds; without the retire/recycle path the second Set would fail.
    EngineSession session(EffectConfig(1, /*delayCap=*/1, /*toneCap=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(4000, 1000.0, kRate, 0.5f);
    VnVoiceHandle handle = PlayMono(session, harkentest::ObjectId(1), sine);

    const VnFeedbackDelayDesc d = DelayDesc(10.0f, 0.5f, 1.0f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&d)));
    ASSERT_EQ(VN_OK, Vn_ClearVoiceEffect(handle, 0));
    Capture apply;
    harkentest::PumpAndCapture(session.Engine(), 1, apply); // audio retires it
    ASSERT_EQ(VN_OK, Vn_Update());                         // logic recycles it

    EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&d)));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(BuiltinEffectPool, VariableOwnerBlocksVoiceReplaceAndClearWithoutMutation) {
    EngineSession session(EffectConfig(1, /*delayCap=*/1, /*toneCap=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(4000, 1000.0, kRate, 0.5f);
    const VnVoiceHandle handle = PlayMono(session, harkentest::ObjectId(11), sine);
    const VnFeedbackDelayDesc delay =
        DelayDesc(10.0f, 0.5f, 1.0f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
                         handle, 0, VN_EFFECT_FEEDBACK_DELAY,
                         AsBase(&delay)));

    harken::Engine* engine = session.Engine();
    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    const std::uint32_t generation =
        static_cast<std::uint32_t>(render >> 32);
    std::uint32_t revision = 0;
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Voice,
        static_cast<std::uint64_t>(render), generation, 0,
        harken::kDelayParamWet, VN_EFFECT_FEEDBACK_DELAY, &revision));
    std::uint32_t dryRevision = 0;
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Voice,
        static_cast<std::uint64_t>(render), generation, 0,
        harken::kDelayParamDry, VN_EFFECT_FEEDBACK_DELAY, &dryRevision));
    const harken::VariableSliceRef owner{0, 1};
    ASSERT_TRUE(engine->effectParamTargets.TryClaim(
        harken::EffectParamTargetStore::Scope::Voice,
        static_cast<std::uint64_t>(render), generation, 0,
        harken::kDelayParamWet, owner, revision));

    const std::uint64_t publishedBefore = engine->commands.PublishedHead();
    const std::uint32_t recordsBefore =
        engine->effectParamTargets.LiveRecordCount();
    const std::uint32_t renderSlot = static_cast<std::uint32_t>(render);
    const harken::EffectInstanceRef instanceBefore =
        engine->voiceEffectInstanceRefs[renderSlot][0];
    const harken::Engine::VoiceEffectSlotConfig configBefore =
        engine->effectSlotConfig[renderSlot][0];
    const std::uint32_t demandBefore =
        engine->effectParamTargets.DemandCount();
    const std::uint32_t reservedBefore =
        engine->effectParamTargets.ReservedRecordCountForTests();
    const std::uint32_t retiringBefore =
        engine->effectParamTargets.RetiringRecordCountForTests();
    const float wetCommittedBefore =
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Voice,
            static_cast<std::uint64_t>(render), 0, harken::kDelayParamWet);
    const float wetResolvedBefore = engine->effectParamTargets.Reduce(
        harken::EffectParamTargetStore::Scope::Voice,
        static_cast<std::uint64_t>(render), 0, harken::kDelayParamWet);
    const float dryCommittedBefore =
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Voice,
            static_cast<std::uint64_t>(render), 0, harken::kDelayParamDry);
    const std::uint32_t delayInUseBefore =
        engine->effectBank.InUseOf(VN_EFFECT_FEEDBACK_DELAY);
    const std::uint32_t delayHighWaterBefore =
        engine->effectBank.HighWaterOf(VN_EFFECT_FEEDBACK_DELAY);
    const std::uint32_t delayRefusalsBefore =
        engine->effectBank.RefusalCountOf(VN_EFFECT_FEEDBACK_DELAY);
    const std::uint32_t toneInUseBefore =
        engine->effectBank.InUseOf(VN_EFFECT_THREE_BAND_TONE);
    const std::uint32_t toneHighWaterBefore =
        engine->effectBank.HighWaterOf(VN_EFFECT_THREE_BAND_TONE);
    const std::uint32_t toneRefusalsBefore =
        engine->effectBank.RefusalCountOf(VN_EFFECT_THREE_BAND_TONE);
    ASSERT_EQ(VN_EFFECT_FEEDBACK_DELAY,
              engine->effectSlotTypes[renderSlot][0]);
    const VnThreeBandToneDesc tone =
        ToneDesc(6.0f, 200.0f, 0.0f, 1000.0f, 1.0f, 0.0f, 6000.0f);
    EXPECT_EQ(VN_BACKPRESSURE,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_THREE_BAND_TONE,
                                AsBase(&tone)));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_ClearVoiceEffect(handle, 0));
    EXPECT_EQ(publishedBefore, engine->commands.PublishedHead());
    EXPECT_EQ(recordsBefore, engine->effectParamTargets.LiveRecordCount());
    EXPECT_EQ(demandBefore, engine->effectParamTargets.DemandCount());
    EXPECT_EQ(reservedBefore,
              engine->effectParamTargets.ReservedRecordCountForTests());
    EXPECT_EQ(retiringBefore,
              engine->effectParamTargets.RetiringRecordCountForTests());
    EXPECT_EQ(VN_EFFECT_FEEDBACK_DELAY,
              engine->effectSlotTypes[renderSlot][0]);
    const harken::EffectInstanceRef instanceAfter =
        engine->voiceEffectInstanceRefs[renderSlot][0];
    EXPECT_EQ(instanceBefore.type, instanceAfter.type);
    EXPECT_EQ(instanceBefore.poolIndex, instanceAfter.poolIndex);
    EXPECT_EQ(instanceBefore.generation, instanceAfter.generation);
    const harken::Engine::VoiceEffectSlotConfig& configAfter =
        engine->effectSlotConfig[renderSlot][0];
    EXPECT_EQ(configBefore.bypass, configAfter.bypass);
    for (std::uint32_t param = 0; param < harken::kDelayParamCount; ++param) {
        EXPECT_FLOAT_EQ(configBefore.params[param], configAfter.params[param]);
    }
    std::uint32_t observedRevision = 0;
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Voice,
        static_cast<std::uint64_t>(render), generation, 0,
        harken::kDelayParamWet, VN_EFFECT_FEEDBACK_DELAY,
        &observedRevision));
    EXPECT_EQ(revision, observedRevision);
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Voice,
        static_cast<std::uint64_t>(render), generation, 0,
        harken::kDelayParamDry, VN_EFFECT_FEEDBACK_DELAY,
        &observedRevision));
    EXPECT_EQ(dryRevision, observedRevision);
    EXPECT_FLOAT_EQ(wetCommittedBefore,
                    engine->effectParamTargets.LastCommitted(
                        harken::EffectParamTargetStore::Scope::Voice,
                        static_cast<std::uint64_t>(render), 0,
                        harken::kDelayParamWet));
    EXPECT_FLOAT_EQ(dryCommittedBefore,
                    engine->effectParamTargets.LastCommitted(
                        harken::EffectParamTargetStore::Scope::Voice,
                        static_cast<std::uint64_t>(render), 0,
                        harken::kDelayParamDry));
    EXPECT_EQ(delayInUseBefore,
              engine->effectBank.InUseOf(VN_EFFECT_FEEDBACK_DELAY));
    EXPECT_EQ(delayHighWaterBefore,
              engine->effectBank.HighWaterOf(VN_EFFECT_FEEDBACK_DELAY));
    EXPECT_EQ(delayRefusalsBefore,
              engine->effectBank.RefusalCountOf(VN_EFFECT_FEEDBACK_DELAY));
    EXPECT_EQ(toneInUseBefore,
              engine->effectBank.InUseOf(VN_EFFECT_THREE_BAND_TONE));
    EXPECT_EQ(toneHighWaterBefore,
              engine->effectBank.HighWaterOf(VN_EFFECT_THREE_BAND_TONE));
    EXPECT_EQ(toneRefusalsBefore,
              engine->effectBank.RefusalCountOf(VN_EFFECT_THREE_BAND_TONE));

    const std::uint64_t exactParamHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_BACKPRESSURE,
              Vn_SetVoiceEffectParam(
                  handle, 0, harken::kDelayParamWet, 0.5f));
    EXPECT_EQ(exactParamHead, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(
        wetResolvedBefore,
        engine->effectParamTargets.Reduce(
            harken::EffectParamTargetStore::Scope::Voice,
            static_cast<std::uint64_t>(render), 0,
            harken::kDelayParamWet));
    EXPECT_TRUE(engine->effectParamTargets.ForeignClaim(
        harken::EffectParamTargetStore::Scope::Voice,
        static_cast<std::uint64_t>(render), generation, 0,
        harken::kDelayParamWet));
    EXPECT_EQ(VN_OK,
              Vn_SetVoiceEffectParam(
                  handle, 0, harken::kDelayParamDry, 0.75f));
    EXPECT_EQ(exactParamHead + 1u, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(
        0.75f,
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Voice,
            static_cast<std::uint64_t>(render), 0,
            harken::kDelayParamDry));

    engine->effectParamTargets.ReleaseClaim(
        harken::EffectParamTargetStore::Scope::Voice,
        static_cast<std::uint64_t>(render), generation, 0,
        harken::kDelayParamWet, owner);
    EXPECT_EQ(VN_OK,
              Vn_SetVoiceEffectParam(
                  handle, 0, harken::kDelayParamWet, 0.625f));
    EXPECT_EQ(exactParamHead + 2u, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(
        0.625f,
        engine->effectParamTargets.Reduce(
            harken::EffectParamTargetStore::Scope::Voice,
            static_cast<std::uint64_t>(render), 0,
            harken::kDelayParamWet));
    const float preparedParams[] = {delay.delayMs, delay.feedback, delay.wet,
                                    delay.dry};
    harken::EffectParamTargetStore::PreparedSeedToken prepared;
    ASSERT_TRUE(engine->effectParamTargets.PrepareInstalledEffectSeed(
        harken::EffectParamTargetStore::Scope::Voice,
        static_cast<std::uint64_t>(render), generation, 0,
        VN_EFFECT_FEEDBACK_DELAY, preparedParams, harken::kDelayParamCount,
        &prepared));
    const std::uint64_t preparedHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_BACKPRESSURE,
              Vn_SetVoiceEffectParam(handle, 0, harken::kDelayParamWet, 0.5f));
    EXPECT_EQ(preparedHead, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(
        0.625f,
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Voice,
            static_cast<std::uint64_t>(render), 0, harken::kDelayParamWet));
    engine->effectParamTargets.CancelPreparedInstalledEffectSeed(&prepared);
    EXPECT_EQ(VN_OK, Vn_ClearVoiceEffect(handle, 0));
}

TEST(BuiltinEffectPool, VoiceEndRecyclesInstances) {
    // A voice that ends with an effect installed must release its instance to
    // the pool, or a one-instance pool would leak and refuse the next Set.
    EngineSession session(EffectConfig(1, /*delayCap=*/1, /*toneCap=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(4000, 1000.0, kRate, 0.5f);

    const VnPCMDesc pcm = MonoDesc(sine, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle first = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    const VnFeedbackDelayDesc d = DelayDesc(10.0f, 0.5f, 1.0f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(first, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&d)));
    ASSERT_EQ(VN_OK, Vn_Stop(first, 0));
    Capture drain;
    harkentest::PumpAndCapture(session.Engine(), 1, drain); // ReleaseSlot retires
    ASSERT_EQ(VN_OK, Vn_Update());                         // recycle

    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(second, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&d)));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(BuiltinEffectPool, InPlaceReplaceRetiresOldInstance) {
    // Two-instance pool: replacing a slot's effect takes a new instance and
    // retires the old, so after a pump + update exactly one is in use and the
    // high-water shows both coexisted during the swap.
    EngineSession session(EffectConfig(1, /*delayCap=*/2, /*toneCap=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(4000, 1000.0, kRate, 0.5f);
    VnVoiceHandle handle = PlayMono(session, harkentest::ObjectId(1), sine);

    const VnFeedbackDelayDesc d = DelayDesc(10.0f, 0.5f, 1.0f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&d)));
    Capture warm;
    harkentest::PumpAndCapture(session.Engine(), 1, warm);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&d))); // in-place replace
    Capture apply;
    harkentest::PumpAndCapture(session.Engine(), 1, apply); // audio retires old
    ASSERT_EQ(VN_OK, Vn_Update());                         // recycle old

    EXPECT_EQ(1u, session.Engine()->effectBank.DelayInUse());
    EXPECT_EQ(2u, session.Engine()->effectBank.DelayHighWater());
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(BuiltinEffectPool, HighWaterSurfacesInSnapshot) {
    EngineSession session(EffectConfig(1, /*delayCap=*/4, /*toneCap=*/4));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(4000, 1000.0, kRate, 0.5f);
    const VnPCMDesc pcm = MonoDesc(sine, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    const VnFeedbackDelayDesc d = DelayDesc(10.0f, 0.5f, 1.0f, 1.0f);
    for (int i = 0; i < 3; ++i) {
        VnVoiceHandle h = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &h));
        ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(h, 0, VN_EFFECT_FEEDBACK_DELAY,
                                           AsBase(&d)));
    }
    Capture warm;
    harkentest::PumpAndCapture(session.Engine(), 1, warm);

    VnSnapshot snap{};
    snap.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snap));
    EXPECT_EQ(3u, snap.delayEffectHighWater);
    EXPECT_EQ(0u, snap.toneEffectHighWater);
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(BuiltinEffectPool, LargeExplicitQuotaDoesNotPreallocateDelayLines) {
    auto config = harkentest::MakeNullConfig(0, kBlock, 384000);
    config.delayEffectPoolCapacity = 65536;
    config.gainEffectPoolCapacity = UINT32_MAX;
    EffectExtraAllocationFault fault;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& bank = session.Engine()->effectBank;
    EXPECT_EQ(65536u, bank.CapacityOf(VN_EFFECT_FEEDBACK_DELAY));
    EXPECT_EQ(0u, harken::detail::EffectPoolTestAccess::ReadyCount(bank, VN_EFFECT_FEEDBACK_DELAY));
    EXPECT_EQ(1u, harken::detail::g_logicAllocFault.remaining);
    EXPECT_EQ(harken::kEffectTypeNone, bank.Acquire(VN_EFFECT_FEEDBACK_DELAY).type);
    EXPECT_EQ(0u, bank.InUseOf(VN_EFFECT_FEEDBACK_DELAY));
}

TEST(BuiltinEffectDrain, ClearingLongTailMidDrainEndsVoiceEarly) {
    // End-to-end through the public command path: a voice carrying a long-tail
    // delay and a short-tail delay, cleared of its long-tail slot after the
    // source has ended, must finish per the surviving short tail — far sooner
    // than if the long tail had been left to drain. Exercises the drain
    // re-derivation as reached from Vn_ClearVoiceEffect, not just the cascade.
    auto runToSilence = [](bool clearLong) -> std::uint32_t {
        harkentest::EngineSession session(EffectConfig(2, /*delayCap=*/2,
                                                      /*toneCap=*/1));
        EXPECT_EQ(VN_OK, session.InitResult());
        const std::vector<float> src =
            harkentest::MakeSine(256, 1000.0, kRate, 0.5f);
        const VnPCMDesc pcm = MonoDesc(src, kRate);
        EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle h = 0;
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &h));
        const VnFeedbackDelayDesc lng =
            DelayDesc(200.0f, 0.0f, 1.0f, 1.0f); // tail ~9600 frames
        const VnFeedbackDelayDesc sht =
            DelayDesc(10.0f, 0.0f, 1.0f, 1.0f); // tail ~480 frames
        EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(h, 0, VN_EFFECT_FEEDBACK_DELAY,
                                           AsBase(&lng)));
        EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(h, 1, VN_EFFECT_FEEDBACK_DELAY,
                                           AsBase(&sht)));
        std::uint32_t blocks = 0;
        for (std::uint32_t i = 0; i < 128; ++i) {
            session.Engine()->nullBackend.Pump(1);
            ++blocks;
            if (clearLong && i == 3) {
                // The 256-frame source has ended (~1 block) and the voice is
                // draining the long tail; cut the long-tail slot mid-drain.
                EXPECT_EQ(VN_OK, Vn_ClearVoiceEffect(h, 0));
            }
            VnSnapshot snap{};
            snap.structSize = sizeof(VnSnapshot);
            EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snap));
            if (snap.activeVoiceCount == 0) {
                break;
            }
        }
        return blocks;
    };

    const std::uint32_t baseline = runToSilence(/*clearLong=*/false);
    const std::uint32_t cleared = runToSilence(/*clearLong=*/true);
    EXPECT_GT(baseline, 30u);     // long tail keeps the voice alive ~38 blocks
    EXPECT_LT(cleared, baseline); // clearing it ends the voice sooner
    EXPECT_LT(cleared, 12u);      // governed by the surviving short tail
}

// --- reset determinism (direct state, unobservable through the API) ---

TEST(BuiltinEffectReset, DelayResetClearsLineAndParams) {
    std::vector<float> line(static_cast<std::size_t>(harken::kMaxChannels) * 64,
                            7.0f);
    harken::DelayEffectState st;
    st.lineBase = line.data();
    st.lineFrames = 64;
    st.sampleRate = kRate;
    harken::InsertEffect fx =
        harken::BuildEffect(VN_EFFECT_FEEDBACK_DELAY, &st);
    ASSERT_NE(nullptr, fx.setParam);
    ASSERT_NE(nullptr, fx.reset);
    fx.setParam(&st, harken::kDelayParamFeedback, 0.9f);
    fx.setParam(&st, harken::kDelayParamWet, 0.7f);
    st.writeIndex = 33;

    fx.reset(&st);
    EXPECT_EQ(0u, st.writeIndex);
    EXPECT_EQ(0.0f, st.feedback);
    EXPECT_EQ(0.0f, st.wet);
    EXPECT_EQ(1.0f, st.dry);
    for (float v : line) {
        EXPECT_EQ(0.0f, v);
    }
}

TEST(BuiltinEffectReset, ToneResetRestoresDefaultsAndClearsMemory) {
    harken::ToneEffectState st;
    st.sampleRate = kRate;
    harken::InsertEffect fx =
        harken::BuildEffect(VN_EFFECT_THREE_BAND_TONE, &st);
    ASSERT_NE(nullptr, fx.reset);
    fx.setParam(&st, harken::kToneParamPeakGainDb, 12.0f);
    st.peakState[0].SetFloat(5.0f, -3.0f);

    fx.reset(&st);
    EXPECT_EQ(0.0f, st.peakGainDb);
    EXPECT_EQ(0.0f, st.peakState[0].Z1());
    EXPECT_EQ(0.0f, st.peakState[0].Z2());
}

TEST(BuiltinTone, CascadedFiniteOverflowRecoversWithoutAffectingOtherChannels) {
    const auto desc = ToneDesc(0.0f, 200.0f, 48.0f, 1000.0f,
                               1.0f, 0.0f, 6000.0f);
    ASSERT_EQ(VN_OK, harken::ValidateEffectDesc(
        VN_EFFECT_THREE_BAND_TONE, AsBase(&desc), kRate));
    for (bool transition : {false, true}) {
        SCOPED_TRACE(transition);
        harken::ToneEffectState state;
        state.sampleRate = kRate;
        auto effect = harken::BuildEffect(VN_EFFECT_THREE_BAND_TONE, &state);
        effect.reset(&state);
        effect.setParam(&state, harken::kToneParamPeakGainDb, 48.0f);
        float left[kBlock + 2], right[kBlock + 2];
        float* channels[] = {left, right};
        for (unsigned block = 0; block < 257; ++block) {
            std::fill_n(left, kBlock + 2, 0.125f);
            std::fill_n(right, kBlock + 2, 0.375f);
            left[0] = right[0] = 17.0f;
            left[kBlock + 1] = right[kBlock + 1] = 19.0f;
            if (block == 0) left[1] = (std::numeric_limits<float>::max)();
            if (transition && block == 1) {
                const auto coeffs = harken::MakePeaking(float(kRate), 1000.0f, 1.0f, 6.0f);
                harken::ToneApplyPreparedParam(&state, harken::kToneParamPeakGainDb,
                    6.0f, coeffs, harken::EstimateBiquadTailFrames(coeffs));
            }
            effect.process(&state, channels, 2, 1, kBlock);
            if (block == 0) EXPECT_FALSE(std::isfinite(left[1]));
            EXPECT_EQ(17.0f, left[0]);
            EXPECT_EQ(19.0f, left[kBlock + 1]);
            EXPECT_EQ(17.0f, right[0]);
            EXPECT_EQ(19.0f, right[kBlock + 1]);
            for (unsigned i = 1; i <= kBlock; ++i) ASSERT_TRUE(std::isfinite(right[i]));
        }
        for (unsigned i = 1; i <= kBlock; ++i) {
            ASSERT_TRUE(std::isfinite(left[i]));
            EXPECT_NEAR(0.125f, left[i], 1e-3f);
            EXPECT_NEAR(0.375f, right[i], 1e-3f);
        }
    }
}

TEST(BuiltinEffectValidation, TonePreparationRejectsUnitCirclePole) {
    float params[harken::kToneParamCount] = {};
    params[harken::kToneParamLowShelfGainDb] = 6.0f;
    params[harken::kToneParamLowShelfFreqHz] = 1.0e-20f;
    harken::BiquadCoeffs coeffs;
    std::uint32_t tailFrames = 0;
    EXPECT_FALSE(harken::PrepareToneSection(
        harken::kToneParamLowShelfFreqHz, params, kRate, &coeffs,
        &tailFrames));
}

} // namespace


TEST(BuiltinDelay, FiniteCancellationPreservesFollowingFilterRecovery) {
    std::vector<float> line(harken::kMaxChannels, 0.0f);
    harken::DelayEffectState state;
    state.lineBase = line.data();
    state.lineFrames = 1;
    state.sampleRate = kRate;
    auto effect = harken::BuildEffect(VN_EFFECT_FEEDBACK_DELAY, &state);
    effect.reset(&state);
    effect.setParam(&state, harken::kDelayParamWet, 2.0f);
    effect.setParam(&state, harken::kDelayParamDry, 2.0f);
    std::vector<float> samples(kBlock), scratch(kBlock);
    float* channels[] = {samples.data()};
    float* dry[] = {scratch.data()};
    const auto fillExtreme = [&] {
        for (unsigned i = 0; i < kBlock; ++i)
            samples[i] = i % 2 ? -2.0e38f : 2.0e38f;
    };
    // Bypass selects dry output while the actual chain helper advances history.
    fillExtreme();
    bool ramping = false;
    float mix = 1.0f;
    harken::ProcessChainSlot(effect, ramping, mix, 1.0f, channels, dry, 1, 0, kBlock);
    for (float value : samples) ASSERT_TRUE(std::isfinite(value));
    fillExtreme();
    ramping = true;
    harken::ProcessChainSlot(effect, ramping, mix, 0.0f, channels, dry, 1, 0, kBlock);
    for (float value : samples) EXPECT_TRUE(std::isfinite(value));
    EXPECT_EQ(0.0f, samples.back());
    const auto coefficients = harken::MakeLowpass(float(kRate), 2000.0f, 0.707f);
    harken::BiquadState filter{};
    harken::BiquadProcess(coefficients, filter, samples.data(), kBlock);
    // The final delayed extreme must also stay representable during recovery.
    effect.setParam(&state, harken::kDelayParamWet, 0.5f);
    effect.setParam(&state, harken::kDelayParamDry, 0.75f);
    for (unsigned block = 0; block < 128; ++block) {
        std::fill(samples.begin(), samples.end(), 0.125f);
        effect.process(&state, channels, 1, 0, kBlock);
        harken::BiquadProcess(coefficients, filter, samples.data(), kBlock);
    }
    for (float value : samples) EXPECT_NEAR(0.15625f, value, 1.0e-5f);
}

TEST(BuiltinDelay, CancellationFallbackKeepsOffsetsChannelsAndTrueOverflow) {
    std::vector<float> line(harken::kMaxChannels, 0.0f);
    harken::DelayEffectState state;
    state.lineBase = line.data();
    state.lineFrames = 1;
    state.sampleRate = kRate;
    auto effect = harken::BuildEffect(VN_EFFECT_FEEDBACK_DELAY, &state);
    effect.reset(&state);
    effect.setParam(&state, harken::kDelayParamWet, 2.0f);
    effect.setParam(&state, harken::kDelayParamDry, 2.0f);
    float left[] = {17.0f, 2.0e38f, 19.0f};
    float right[] = {23.0f, -2.0e38f, 29.0f};
    float* channels[] = {left, right};
    line[0] = -left[1];
    line[1] = -right[1];
    effect.process(&state, channels, 2, 1, 1);
    EXPECT_EQ(0.0f, left[1]);
    EXPECT_EQ(0.0f, right[1]);
    EXPECT_EQ(17.0f, left[0]);
    EXPECT_EQ(19.0f, left[2]);
    EXPECT_EQ(23.0f, right[0]);
    EXPECT_EQ(29.0f, right[2]);
    left[1] = line[0] = 2.0e38f;
    effect.process(&state, channels, 1, 1, 1);
    EXPECT_TRUE(std::isinf(left[1]));
}

TEST(BuiltinDelay, InvalidFramesPreserveValidTailAndAutomaticallyRecover) {
    constexpr unsigned delay = 4;
    const float faults[] = {std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
    for (float feedback : {0.0f, 0.5f}) {
        for (float fault : faults) {
            std::vector<float> line(delay * harken::kMaxChannels, 0.25f);
            std::vector<float> referenceLine = line;
            harken::DelayEffectState state, reference;
            state.lineBase = line.data();
            reference.lineBase = referenceLine.data();
            state.lineFrames = reference.lineFrames = delay;
            state.delaySamples = reference.delaySamples = delay;
            state.feedback = reference.feedback = feedback;
            state.dry = reference.dry = 0.75f;
            state.wet = reference.wet = 0.5f;
            auto effect = harken::BuildEffect(VN_EFFECT_FEEDBACK_DELAY, &state);
            // One stale invalid cell must not erase other cells or channels.
            line[1] = fault;
            referenceLine[1] = 0.0f;
            for (unsigned frame = 0; frame < 32; ++frame) {
                float left[] = {17.0f, frame == 0 ? fault : 0.125f, 19.0f};
                float right[] = {23.0f, 0.375f, 29.0f};
                float cleanLeft[] = {17.0f, frame == 0 ? 0.0f : 0.125f, 19.0f};
                float cleanRight[] = {23.0f, 0.375f, 29.0f};
                float* channels[] = {left, right};
                float* clean[] = {cleanLeft, cleanRight};
                effect.process(&state, channels, 2, 1, 1);
                effect.process(&reference, clean, 2, 1, 1);
                ASSERT_TRUE(std::isfinite(left[1]));
                EXPECT_FLOAT_EQ(cleanLeft[1], left[1]);
                EXPECT_FLOAT_EQ(cleanRight[1], right[1]);
                if (frame == 0) EXPECT_FLOAT_EQ(0.125f, left[1]);
                EXPECT_EQ(17.0f, left[0]);
                EXPECT_EQ(19.0f, left[2]);
                EXPECT_EQ(23.0f, right[0]);
                EXPECT_EQ(29.0f, right[2]);
            }
            for (unsigned i = 0; i < delay * 2; ++i)
                EXPECT_FLOAT_EQ(referenceLine[i], line[i]);
        }
    }
}
