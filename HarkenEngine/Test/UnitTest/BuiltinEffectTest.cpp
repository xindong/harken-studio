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
#include "harken/dsp/Biquad.h"
#include "harken/render/BuiltinEffects.h"

#include <cmath>
#include <cstdint>
#include <vector>
#include <limits>
#include <utility>

namespace harken::detail {
struct EffectPoolTestAccess {
    static bool SeedFreeGeneration(EffectPoolBank& bank, const EffectInstanceRef& disposed,
                                   std::uint64_t generation) {
        auto* slot = bank.SlotFor(disposed.type);
        if (slot == nullptr || disposed.poolIndex >= slot->capacity ||
            slot->states[disposed.poolIndex].load(std::memory_order_acquire) !=
                EffectPoolBank::InstanceState::Free) return false;
        slot->generations[disposed.poolIndex] = generation;
        return true;
    }
};
}

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

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
    ASSERT_TRUE(bank.RetireQueue().TryPush(ref));
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

TEST(BuiltinEffectPool, DelayLineByteCeilingRejectsOversizedConfig) {
    // The instance-count ceiling alone would accept a large delay pool at a
    // high sample rate; the byte-level ceiling rejects the combination before
    // Init attempts a multi-gigabyte allocation (which would otherwise throw
    // bad_alloc out of the C API). A modest pool at a normal rate still Inits,
    // so the check discriminates rather than blanket-refusing.
    VnConfig oversized = harkentest::MakeNullConfig(0, kBlock, 384000);
    oversized.delayEffectPoolCapacity = 64; // 64 * 384000 * 8ch * 4B ~= 786 MB
    oversized.toneEffectPoolCapacity = 1;
    EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&oversized)); // no engine constructed

    VnConfig ok = harkentest::MakeNullConfig(0, kBlock, kRate);
    ok.delayEffectPoolCapacity = 4; // 4 * 48000 * 8ch * 4B ~= 6 MB
    ok.toneEffectPoolCapacity = 1;
    harkentest::EngineSession session(ok);
    EXPECT_EQ(VN_OK, session.InitResult());
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
    st.peakState[0].z1 = 5.0f;
    st.peakState[0].z2 = -3.0f;

    fx.reset(&st);
    EXPECT_EQ(0.0f, st.peakGainDb);
    EXPECT_EQ(0.0f, st.peakState[0].z1);
    EXPECT_EQ(0.0f, st.peakState[0].z2);
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
