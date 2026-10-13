// Golden case: the acceptance mechanics of a Variable sweeping a voice's
// lowpass cutoff through a curve binding. One Variable is bound via a
// linear curve (value 0 -> 500 Hz, value 1 -> 20 kHz) to the lowpass
// target of a voice playing a 4 kHz sine; sweeping the value must produce
// a measurable change in the rendered output.
//
// Evidence is two-sided by design:
//   - Parameter evidence: the resolved combine value equals the curve's
//     exact endpoints (500 / 20000 Hz), and repeated updates without a
//     value change commit nothing further (changed-only).
//   - Render evidence: a 4 kHz tone through the 500 Hz Butterworth
//     lowpass (3 octaves above cutoff, ~ -36 dB) collapses to a small
//     RMS, while the 20 kHz cutoff leaves it essentially untouched; the
//     open-cutoff capture also passes the silence guard, so the
//     comparison can never green on two silent buffers.
//
// Expected magnitudes (2nd-order Butterworth, |H| = 1/sqrt(1+(f/fc)^4)):
//   fc = 500 Hz:  |H(4k)| ~ 0.0156 -> RMS ~ 0.0055 (amplitude 0.5)
//   fc = 20 kHz:  |H(4k)| ~ 0.999  -> RMS ~ 0.353
// Thresholds leave wide margin on both sides and a 10x separation.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <cmath>
#include <memory>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;
constexpr double kRate = 48000.0;
constexpr double kToneHz = 4000.0;
constexpr VnVariableId kCutoffVariable = harkentest::ObjectId(42);

float BlockRms(const std::vector<float>& s, std::size_t begin,
               std::size_t end) {
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i) {
        sum += static_cast<double>(s[i]) * static_cast<double>(s[i]);
    }
    return static_cast<float>(
        std::sqrt(sum / static_cast<double>(end - begin)));
}

TEST(VariableSweepGolden, LowpassSweepAudiblyReshapesTheRender) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {kCutoffVariable}, 0.0f, 1.0f).status);

    const std::vector<float> sine =
        harkentest::MakeSine(48000, kToneHz, kRate, 0.5f);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // Binding assembly through the engine-internal surface: the M1 public
    // API drives the Variable only.
    harken::Curve curve;
    const harken::CurvePoint pts[] = {{0.0f, 500.0f}, {1.0f, 20000.0f}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
    ASSERT_TRUE(engine->params.BindVoiceTarget(kCutoffVariable,
        harkentest::RenderHandle(engine, handle),
        harken::kTargetVoiceLowpassCutoff, curve));

    // Sweep low: the Variable resolves through the curve to 500 Hz once the
    // accepted write reaches Applied at its effective quantum.
    ASSERT_EQ(VN_OK, Vn_SetValue(kCutoffVariable, 0.0f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_FLOAT_EQ(500.0f,
                    engine->params.ResolvedValue(harkentest::RenderHandle(
                        engine, handle), harken::kTargetVoiceLowpassCutoff));
    const std::uint64_t commitsAfterLow = engine->params.CommitCount();
    EXPECT_GE(commitsAfterLow, 1u);
    // Changed-only: pumping the API again without a value change commits
    // nothing further.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(commitsAfterLow, engine->params.CommitCount());

    Capture lowSweep;
    harkentest::PumpAndCapture(engine, 4, lowSweep);
    // Filter transient has settled by the last block (~16 ms in).
    const float rmsLow =
        BlockRms(lowSweep.left, 3 * kBlockFrames, 4 * kBlockFrames);

    // Sweep high: same binding, value 1 resolves to 20 kHz.
    ASSERT_EQ(VN_OK, Vn_SetValue(kCutoffVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_FLOAT_EQ(20000.0f,
                    engine->params.ResolvedValue(harkentest::RenderHandle(
                        engine, handle), harken::kTargetVoiceLowpassCutoff));

    Capture highSweep;
    harkentest::PumpAndCapture(engine, 4, highSweep);
    HARKENTEST_ASSERT_NON_SILENT(highSweep.left);
    const float rmsHigh =
        BlockRms(highSweep.left, 3 * kBlockFrames, 4 * kBlockFrames);

    // The sweep is measurable, in the physically expected direction and
    // magnitude: near-full tone open, deeply attenuated closed, an order
    // of magnitude apart.
    EXPECT_GT(rmsHigh, 0.3f);
    EXPECT_LT(rmsLow, 0.02f);
    EXPECT_GT(rmsHigh, 10.0f * rmsLow);
}

} // namespace
