// A pooled effect instance, once recycled and re-acquired, must begin
// audibly clean — the released contract is that the logic-side acquire path
// resets an instance before it re-enters a chain, so a reused instance carries
// none of its previous life's delay-line contents or filter memory. This is
// proven end to end through the public C API on the null backend: a fresh
// instance's rendered output must equal a recycled instance's output for the
// same voice, effect, and deterministic signal, byte for byte. The comparison
// is in-memory within one process (same binary), so bit-exactness is the right
// bar and no golden file is involved. The test drives only the public path
// (raw voice start / Vn_SetVoiceEffect / Vn_ClearVoiceEffect / Vn_Stop / Vn_Update)
// and never calls the instance reset itself — the production acquire/reset
// path is exactly what is under test.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kCaptureBlocks = 8;
// The source must outlive the capture window so the voice is still alive when
// the effect is cleared. A zero-tail effect (the tone effect reports no tail)
// lets the voice complete and auto-retire the instant its source is exhausted;
// clearing then hits an ended voice and returns VN_STALE_HANDLE before the
// recycle can be driven. The delay effect's feedback tail happens to keep its
// voice alive, but the harness must not depend on that. These extra source
// frames are consumed only after the capture window, so they never enter the
// fresh-vs-reused byte comparison.
constexpr std::uint32_t kSourceBlocks = kCaptureBlocks + 4;
// Enough blocks after Stop for the cleared voice to fully release, so it
// contributes silence to the reused-instance capture (no output overlap).
constexpr std::uint32_t kDrainBlocks = 8;

// Single-instance pools force the recycled slot to be the one re-acquired, so
// a failure to reset it shows up in the reused-instance render.
VnConfig ReuseConfig() {
    VnConfig config = harkentest::MakeNullConfig(0, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.delayEffectPoolCapacity = 1;
    config.toneEffectPoolCapacity = 1;
    config.limiterEffectPoolCapacity = 1;
    return config;
}

const VnEffectDesc* AsBase(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

VnFeedbackDelayDesc DelayDesc() {
    VnFeedbackDelayDesc d{};
    d.structSize = sizeof(VnFeedbackDelayDesc);
    d.delayMs = 10.0f;
    d.feedback = 0.85f; // strong feedback thoroughly dirties the delay line
    d.wet = 1.0f;
    d.dry = 1.0f;
    return d;
}

VnThreeBandToneDesc ToneDesc() {
    VnThreeBandToneDesc t{};
    t.structSize = sizeof(VnThreeBandToneDesc);
    t.lowShelfGainDb = 9.0f;
    t.lowShelfFreqHz = 200.0f;
    t.peakGainDb = 9.0f;
    t.peakFreqHz = 1000.0f;
    t.peakQ = 4.0f; // narrow, high-gain peak keeps biquad memory ringing
    t.highShelfGainDb = 9.0f;
    t.highShelfFreqHz = 6000.0f;
    return t;
}

VnLimiterDesc LimiterDesc() {
    VnLimiterDesc limiter{};
    limiter.structSize = sizeof(VnLimiterDesc);
    limiter.ceilingDb = -12.0f;
    limiter.releaseMs = 100.0f;
    return limiter;
}

// Deterministic mono stimulus: a unit impulse plus a bounded pseudo-random
// tail, exactly representable so the driving signal is identical every run.
std::vector<float> DirtySignal() {
    std::vector<float> s(static_cast<std::size_t>(kSourceBlocks) * kBlock,
                         0.0f);
    std::uint32_t rng = 0x51ED2C0Fu; // fixed seed
    for (std::size_t i = 0; i < s.size(); ++i) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const std::int32_t bits =
            static_cast<std::int32_t>(rng & 0xFFFFu) - 32768;
        s[i] = static_cast<float>(bits) / 262144.0f; // /2^18 -> [-0.125,0.125)
    }
    s[0] = 1.0f;
    return s;
}

float BlockPeak(const std::vector<float>& buf, std::size_t begin) {
    float peak = 0.0f;
    for (std::size_t i = begin; i < buf.size(); ++i) {
        peak = std::fmax(peak, std::fabs(buf[i]));
    }
    return peak;
}

void ExpectBitExact(const std::vector<float>& a, const std::vector<float>& b,
                    const char* channel) {
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::uint32_t wa = 0;
        std::uint32_t wb = 0;
        std::memcpy(&wa, &a[i], sizeof(wa));
        std::memcpy(&wb, &b[i], sizeof(wb));
        EXPECT_EQ(wa, wb) << "reused-instance " << channel << " sample " << i
                          << " differs from fresh: fresh " << a[i] << ", reused "
                          << b[i];
    }
}

// Renders a fresh instance and a recycled-then-reacquired instance of the same
// effect through the public API and asserts the two outputs match byte-exactly.
void ExpectReuseMatchesFresh(VnEffectType type, const VnEffectDesc* desc) {
    EngineSession session(ReuseConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> signal = DirtySignal();
    const VnPCMDesc pcm = MonoDesc(signal, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));

    // Run A — fresh instance (pool init state), dirtied by the render.
    VnVoiceHandle fresh = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &fresh));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(fresh, 0, type, desc));
    Capture freshOut;
    harkentest::PumpAndCapture(session.Engine(), kCaptureBlocks, freshOut);

    // Recycle the single instance back to its pool via the production path:
    // clear (audio retires), stop the voice, drain (release completes), then
    // Vn_Update (logic recycles). No manual reset — that is the point.
    ASSERT_EQ(VN_OK, Vn_ClearVoiceEffect(fresh, 0));
    ASSERT_EQ(VN_OK, Vn_Stop(fresh, 0));
    Capture drain;
    harkentest::PumpAndCapture(session.Engine(), kDrainBlocks, drain);
    ASSERT_EQ(VN_OK, Vn_Update());
    // The first voice must be fully silent before the reuse capture, or its
    // output would overlap and invalidate the comparison.
    ASSERT_LT(BlockPeak(drain.left, drain.left.size() - kBlock), 1e-6f)
        << "first voice still audible before reuse capture — raise kDrainBlocks";

    // Run B — the same instance, reacquired. A VN_OK here proves the recycled
    // instance was reused (single-instance pool); the contract says it is now
    // clean.
    VnVoiceHandle reused = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &reused));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(reused, 0, type, desc));
    Capture reusedOut;
    harkentest::PumpAndCapture(session.Engine(), kCaptureBlocks, reusedOut);

    HARKENTEST_ASSERT_NON_SILENT(freshOut.left);
    ExpectBitExact(freshOut.left, reusedOut.left, "left");
    ExpectBitExact(freshOut.right, reusedOut.right, "right");
}

TEST(EffectRecycleClean, DelayInstanceStartsCleanAfterRecycle) {
    const VnFeedbackDelayDesc desc = DelayDesc();
    ExpectReuseMatchesFresh(VN_EFFECT_FEEDBACK_DELAY, AsBase(&desc));
}

TEST(EffectRecycleClean, ToneInstanceStartsCleanAfterRecycle) {
    const VnThreeBandToneDesc desc = ToneDesc();
    ExpectReuseMatchesFresh(VN_EFFECT_THREE_BAND_TONE, AsBase(&desc));
}

TEST(EffectRecycleClean, LimiterInstanceStartsCleanAfterRecycle) {
    const VnLimiterDesc desc = LimiterDesc();
    ExpectReuseMatchesFresh(VN_EFFECT_LIMITER, AsBase(&desc));
}

} // namespace
