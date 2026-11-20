// Recorded-baseline regression cases: deterministic null-backend
// coverage spans the original acceptance flows plus stream, effect-chain,
// and combined-scene paths.
// scenarios — spatial flyby, bus ramp + fade-stop, Variable-driven
// lowpass sweep — rendered fresh and compared block by block against
// recorded per-block master-output peaks. Where the analytic golden
// cases prove closed forms, these cases freeze the full acceptance
// mechanics end to end so an unintended behavior change in any layer
// shows up as a peak-envelope diff.
//
// Baseline format (`<scenario>.f32peaks`, binary little-endian — raw
// native byte order on every platform this project targets):
//   uint32 blockCount, then per block two float32 values — the peak
//   absolute sample of the left and right master output channels.
// Comparison tolerance is 1e-4 absolute per value: one recording must
// hold across both build toolchains (MSVC and Clang codegen differ), so
// bit-exactness is deliberately not required.
//
// Multichannel scenarios (5.1 / 7.1 mixes and folds) record every device
// channel, so they use a self-describing sibling format
// (`<scenario>.f32mcpeaks`, binary little-endian):
//   uint32 blockCount, uint32 channelCount, then per block channelCount
//   float32 peak-absolute values in the device channel order (Appendix A).
// The stereo file format remains unchanged. Intentional sound-law revisions
// update their recorded values with evidence in the shared baseline changelog.
//
// Recording mode: run with HARKEN_WRITE_BASELINES=1 to (re)write the
// baseline files from the current render; the test then passes with a
// "baseline written" notice. Default mode requires the file to exist.
// Every baseline file change lands with a same-commit entry in
// HarkenEngine/Test/Baselines/CHANGELOG.md.
#include "Common/ObjectIdTestSupport.h"
#include "Common/StreamBuilder.h"
#include "Common/TestUtil.h"

#include "harken/param/ParamTargets.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#ifndef HARKEN_BASELINE_DIR
#error "HARKEN_BASELINE_DIR must point at HarkenEngine/Test/Baselines"
#endif

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using harkentest::EngineSession;
using harkentest::InterleavedDesc;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;
using harkentest::StreamSpec;
using harkentest::Vec3;

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
// Scenario length: long enough for every scripted transition (ramps,
// fade-out tail) to complete, short enough to keep files and runtime
// negligible.
constexpr double kScenarioSeconds = 1.5;
constexpr float kPeakTolerance = 1e-4f;
constexpr VnVariableId kCutoffVariable = harkentest::ObjectId(42);
constexpr VnAttenuationId kFlybyProfile = harkentest::ObjectId(9);

std::string TempPath(const std::string& name) {
    return std::string(HARKEN_TEMP_DIR "/") + name;
}

VnConfig SceneNullConfig(std::uint32_t fxSlots = 1,
                         std::uint32_t maxStreams = 0) {
    VnConfig config = MakeNullConfig(0, 256, 48000);
    config.fxSlotsPerVoice = fxSlots;
    config.delayEffectPoolCapacity = 4;
    config.toneEffectPoolCapacity = 4;
    config.streamRingFrames = 2048;
    config.maxStreams = maxStreams;
    config.prebufferMs = 1;
    config.starvationCooldownBlocks = 4;
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

// --- deterministic sources ---
// Generator math intentionally duplicated from the host demo's synth
// path (tests never include host code): the baselines then freeze the
// same spectra the live listen accepted.

// Fixed-seed xorshift32 broadband noise. The 24-bit slice mapping keeps
// every value exactly representable in float and inside [-1, 1).
std::vector<float> MakeSeededNoise(std::size_t frames, float amplitude) {
    std::vector<float> s(frames);
    std::uint32_t rng = 0x12345678u;
    for (std::size_t n = 0; n < frames; ++n) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const float uniform =
            static_cast<float>(rng >> 8) * (1.0f / 8388608.0f) - 1.0f;
        s[n] = amplitude * uniform;
    }
    return s;
}

// Harmonic stack: harmonics 1..12 of baseHz at amplitude 1/k (saw-like,
// band-limited far below Nyquist), normalized to the requested peak.
std::vector<float> MakeHarmonicStack(std::size_t frames, double baseHz,
                                     float peakAmplitude) {
    std::vector<float> s(frames);
    float rawPeak = 0.0f;
    for (std::size_t n = 0; n < frames; ++n) {
        double sum = 0.0;
        for (int k = 1; k <= 12; ++k) {
            sum += std::sin(2.0 * kPi * baseHz * k *
                            static_cast<double>(n) / kRate) /
                   static_cast<double>(k);
        }
        const float sample = static_cast<float>(sum);
        if (std::fabs(sample) > rawPeak) {
            rawPeak = std::fabs(sample);
        }
        s[n] = sample;
    }
    const float gain = rawPeak > 0.0f ? peakAmplitude / rawPeak : 0.0f;
    for (float& sample : s) {
        sample *= gain;
    }
    return s;
}

void NormalizePeak(std::vector<float>* samples, float targetPeak) {
    float peak = 0.0f;
    for (float sample : *samples) {
        if (std::fabs(sample) > peak) {
            peak = std::fabs(sample);
        }
    }
    if (peak <= 0.0f) {
        return;
    }
    const float gain = targetPeak / peak;
    for (float& sample : *samples) {
        sample *= gain;
    }
}

std::vector<float> MakeDistinctSignal(std::size_t frames) {
    std::vector<float> s(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double filePosition =
            frames > 1 ? static_cast<double>(i) /
                             static_cast<double>(frames - 1)
                       : 0.0;
        const double contour =
            0.045 +
            0.055 *
                (0.5 + 0.5 *
                           std::sin(2.0 * kPi *
                                        static_cast<double>(i) / 3072.0 +
                                    0.35)) +
            0.025 * filePosition;
        const int centered = static_cast<int>((i * 37u) % 97u) - 48;
        s[i] = static_cast<float>(contour) *
               (static_cast<float>(centered) / 48.0f);
    }
    return s;
}

std::vector<float> MakeMiniMusic(std::size_t frames) {
    std::vector<float> s(frames);
    const double notes[] = {220.0, 261.625565, 329.627557, 391.995436};
    constexpr double noteSeconds = 0.20;
    for (std::size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / kRate;
        const int note =
            static_cast<int>(t / noteSeconds) %
            static_cast<int>(sizeof(notes) / sizeof(notes[0]));
        const double pos = std::fmod(t, noteSeconds) / noteSeconds;
        const double env = std::sin(kPi * pos);
        const double f = notes[note];
        s[n] = static_cast<float>(
            env * (std::sin(2.0 * kPi * f * t) +
                   0.35 * std::sin(2.0 * kPi * 2.0 * f * t)));
    }
    NormalizePeak(&s, 0.35f);
    return s;
}

std::vector<float> MakeMiniAmbience(std::size_t frames) {
    constexpr double loopSeconds = 0.50;
    std::vector<float> s(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / kRate;
        const double phase = t / loopSeconds;
        const double motion = 0.72 + 0.12 * std::sin(2.0 * kPi * phase);
        s[n] = static_cast<float>(
            motion *
            (0.55 * std::sin(2.0 * kPi * 48.0 * t + 0.2) +
             0.30 * std::sin(2.0 * kPi * 96.0 * t + 1.1)));
    }
    NormalizePeak(&s, 0.22f);
    return s;
}

std::vector<float> MakeMiniOneShot(std::size_t frames) {
    std::vector<float> s(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / kRate;
        const double attack = t < 0.003 ? t / 0.003 : 1.0;
        const double env = attack * std::exp(-12.0 * t);
        s[n] = static_cast<float>(
            env * (std::sin(2.0 * kPi * 880.0 * t) +
                   0.35 * std::sin(2.0 * kPi * 1760.0 * t)));
    }
    NormalizePeak(&s, 0.80f);
    return s;
}

// --- baseline file plumbing ---

std::string BaselinePath(const char* scenario) {
    return std::string(HARKEN_BASELINE_DIR "/") + scenario + ".f32peaks";
}

bool WriteBaselinesRequested() {
    const char* env = std::getenv("HARKEN_WRITE_BASELINES");
    return env != nullptr && env[0] == '1' && env[1] == '\0';
}

bool WriteBaseline(const std::string& path,
                   const std::vector<float>& peaks) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    const std::uint32_t blockCount =
        static_cast<std::uint32_t>(peaks.size() / 2);
    out.write(reinterpret_cast<const char*>(&blockCount),
              sizeof(blockCount));
    out.write(reinterpret_cast<const char*>(peaks.data()),
              static_cast<std::streamsize>(peaks.size() * sizeof(float)));
    return out.good();
}

bool ReadBaseline(const std::string& path, std::vector<float>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::uint32_t blockCount = 0;
    in.read(reinterpret_cast<char*>(&blockCount), sizeof(blockCount));
    // Corruption guard: a plausible file never holds more than a few
    // hundred blocks; refuse absurd counts instead of allocating them.
    if (!in.good() || blockCount > (1u << 20)) {
        return false;
    }
    out->resize(static_cast<std::size_t>(blockCount) * 2);
    in.read(reinterpret_cast<char*>(out->data()),
            static_cast<std::streamsize>(out->size() * sizeof(float)));
    if (in.gcount() !=
        static_cast<std::streamsize>(out->size() * sizeof(float))) {
        return false;
    }
    // A corrupt file must read as unreadable, which the compare path
    // reports with the re-record hint: refuse non-finite payload values
    // and trailing bytes beyond the declared block count alike.
    for (const float value : *out) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return in.peek() == std::ifstream::traits_type::eof();
}

// --- scenario driving ---

std::uint32_t ScenarioBlockCount(harken::Engine* engine, double seconds) {
    const double frames =
        static_cast<double>(engine->nullBackend.BlockFrames());
    return static_cast<std::uint32_t>(
        std::ceil(seconds * kRate / frames));
}

std::uint32_t ScenarioBlockCount(harken::Engine* engine) {
    return ScenarioBlockCount(engine, kScenarioSeconds);
}

// Renders one block on the null backend and appends its per-channel peak
// absolute sample to `peaks`, one value per captured channel in device
// channel order. `captureChannels` is how many leading device channels to
// record: the default of 2 (front L/R) records the stereo envelope every
// M1-M4 scenario recorded — the two-loop reduction below yields the exact
// same per-channel max as the prior interleaved form, so those envelopes and
// their on-disk .f32peaks files are bit-identical. A `captureChannels` of 0
// means "every device channel", the multichannel scenarios' N-channel form.
void PumpBlockPeaks(harken::Engine* engine, std::vector<float>* peaks,
                    std::uint32_t captureChannels = 2) {
    engine->nullBackend.Pump(1);
    const float* block = engine->nullBackend.LastBlock();
    const std::uint32_t frames = engine->nullBackend.BlockFrames();
    const std::uint32_t channels = engine->nullBackend.Channels();
    const std::uint32_t capture =
        captureChannels == 0 ? channels : captureChannels;
    for (std::uint32_t c = 0; c < capture; ++c) {
        float peak = 0.0f;
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float v = std::fabs(block[i * channels + c]);
            if (v > peak) {
                peak = v;
            }
        }
        peaks->push_back(peak);
    }
}

// Records or verifies one scenario's peak envelope, per the mode.
void CompareOrRecord(const char* scenario, const std::vector<float>& peaks) {
    // Poison guard: a NaN or Inf peak means the render itself broke;
    // such an envelope must never be recorded nor compared.
    for (std::size_t i = 0; i < peaks.size(); ++i) {
        ASSERT_TRUE(std::isfinite(peaks[i]))
            << "scenario '" << scenario
            << "' produced a non-finite peak at block " << (i / 2)
            << (i % 2 == 0 ? " left" : " right") << " channel";
    }
    // Silence guard: an all-quiet render must never be recorded as a
    // baseline nor compared green against one.
    ASSERT_GT(harkentest::Peak(peaks), harkentest::kSilenceFloor)
        << "silence-guard: scenario '" << scenario << "' rendered silent";

    const std::string path = BaselinePath(scenario);
    if (WriteBaselinesRequested()) {
        ASSERT_TRUE(WriteBaseline(path, peaks))
            << "cannot write baseline file: " << path;
        std::printf("baseline written: %s\n", path.c_str());
        return;
    }

    std::vector<float> expected;
    ASSERT_TRUE(ReadBaseline(path, &expected))
        << "baseline missing or unreadable: " << path
        << " — record it by rerunning with HARKEN_WRITE_BASELINES=1";
    ASSERT_EQ(expected.size(), peaks.size())
        << "block count changed for '" << scenario
        << "': the scenario script or engine block layout moved — "
           "investigate before re-recording";
    for (std::size_t i = 0; i < peaks.size(); ++i) {
        EXPECT_NEAR(expected[i], peaks[i], kPeakTolerance)
            << "scenario '" << scenario << "' block " << (i / 2)
            << (i % 2 == 0 ? " left" : " right") << " channel peak";
    }
}

// --- multichannel baseline file plumbing (.f32mcpeaks) ---
// A distinct extension from the stereo `.f32peaks` files so the two formats can
// never be cross-read; the channel count rides in the file header, so a
// scenario whose device width changed is caught as a block/channel mismatch.

std::string BaselinePathMulti(const char* scenario) {
    return std::string(HARKEN_BASELINE_DIR "/") + scenario + ".f32mcpeaks";
}

bool WriteBaselineMulti(const std::string& path,
                        const std::vector<float>& peaks,
                        std::uint32_t channelCount) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || channelCount == 0) {
        return false;
    }
    const std::uint32_t blockCount =
        static_cast<std::uint32_t>(peaks.size() / channelCount);
    out.write(reinterpret_cast<const char*>(&blockCount), sizeof(blockCount));
    out.write(reinterpret_cast<const char*>(&channelCount),
              sizeof(channelCount));
    out.write(reinterpret_cast<const char*>(peaks.data()),
              static_cast<std::streamsize>(peaks.size() * sizeof(float)));
    return out.good();
}

bool ReadBaselineMulti(const std::string& path, std::vector<float>* out,
                       std::uint32_t* channelCountOut) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::uint32_t blockCount = 0;
    std::uint32_t channelCount = 0;
    in.read(reinterpret_cast<char*>(&blockCount), sizeof(blockCount));
    in.read(reinterpret_cast<char*>(&channelCount), sizeof(channelCount));
    // Corruption guard: refuse absurd counts instead of allocating them. A
    // plausible file never holds more than a few hundred blocks, and 8 (7.1)
    // is the widest channel layout this project targets.
    if (!in.good() || blockCount > (1u << 20) || channelCount == 0 ||
        channelCount > 8) {
        return false;
    }
    out->resize(static_cast<std::size_t>(blockCount) * channelCount);
    in.read(reinterpret_cast<char*>(out->data()),
            static_cast<std::streamsize>(out->size() * sizeof(float)));
    if (in.gcount() !=
        static_cast<std::streamsize>(out->size() * sizeof(float))) {
        return false;
    }
    // A corrupt file must read as unreadable: refuse non-finite payload and
    // trailing bytes beyond the declared block/channel product alike.
    for (const float value : *out) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    if (in.peek() != std::ifstream::traits_type::eof()) {
        return false;
    }
    *channelCountOut = channelCount;
    return true;
}

// Records or verifies one multichannel scenario's per-channel peak envelope.
// `peaks` is block-major, channelCount values per block in device channel order.
void CompareOrRecordMulti(const char* scenario, const std::vector<float>& peaks,
                          std::uint32_t channelCount) {
    ASSERT_GT(channelCount, 0u) << "scenario '" << scenario
                                << "' has no device channels";
    ASSERT_EQ(0u, peaks.size() % channelCount)
        << "scenario '" << scenario
        << "' peak count is not a whole number of blocks";
    // Poison guard: a NaN or Inf peak means the render itself broke.
    for (std::size_t i = 0; i < peaks.size(); ++i) {
        ASSERT_TRUE(std::isfinite(peaks[i]))
            << "scenario '" << scenario << "' produced a non-finite peak at "
            << "block " << (i / channelCount) << " channel "
            << (i % channelCount);
    }
    // Silence guard: an all-quiet render must never be recorded nor compared
    // green (individual channels may legitimately be silent — e.g. the LFE,
    // never fed by panning — so the guard is on the whole envelope's peak).
    ASSERT_GT(harkentest::Peak(peaks), harkentest::kSilenceFloor)
        << "silence-guard: scenario '" << scenario << "' rendered silent";

    const std::string path = BaselinePathMulti(scenario);
    if (WriteBaselinesRequested()) {
        ASSERT_TRUE(WriteBaselineMulti(path, peaks, channelCount))
            << "cannot write baseline file: " << path;
        std::printf("baseline written: %s (%u channels)\n", path.c_str(),
                    channelCount);
        return;
    }

    std::vector<float> expected;
    std::uint32_t expectedChannels = 0;
    ASSERT_TRUE(ReadBaselineMulti(path, &expected, &expectedChannels))
        << "baseline missing or unreadable: " << path
        << " — record it by rerunning with HARKEN_WRITE_BASELINES=1";
    ASSERT_EQ(channelCount, expectedChannels)
        << "channel count changed for '" << scenario
        << "': the device layout moved — investigate before re-recording";
    ASSERT_EQ(expected.size(), peaks.size())
        << "block count changed for '" << scenario
        << "': the scenario script or engine block layout moved — "
           "investigate before re-recording";
    for (std::size_t i = 0; i < peaks.size(); ++i) {
        EXPECT_NEAR(expected[i], peaks[i], kPeakTolerance)
            << "scenario '" << scenario << "' block " << (i / channelCount)
            << " channel " << (i % channelCount) << " peak";
    }
}

// A null-backend scene config at an explicit mix / device channel layout,
// carrying the same effect / stream / pool sizing as SceneNullConfig.
VnConfig SceneNullConfigLayout(std::uint32_t mixLayout,
                               std::uint32_t deviceLayout,
                               std::uint32_t fxSlots = 1,
                               std::uint32_t maxStreams = 0) {
    VnConfig config = SceneNullConfig(fxSlots, maxStreams);
    config.mixChannelLayout = mixLayout;
    config.deviceChannelLayout = deviceLayout;
    return config;
}

// A `channels`-channel interleaved bed: each channel carries a distinct
// deterministic tone (a per-channel base frequency, gently amplitude-shaped)
// so every mix-layout channel — front, center, LFE, surround — holds unique,
// non-silent content that the pan/fold path routes or folds distinctly.
std::vector<float> MakeChannelBed(std::size_t frames, std::uint32_t channels) {
    std::vector<float> s(frames * channels);
    for (std::size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / kRate;
        for (std::uint32_t c = 0; c < channels; ++c) {
            const double freq = 110.0 + 55.0 * static_cast<double>(c);
            const double amp = 0.12 + 0.02 * static_cast<double>(c);
            const double motion = 0.85 + 0.15 * std::sin(2.0 * kPi * 0.5 * t +
                                                          0.3 * c);
            s[n * channels + c] = static_cast<float>(
                amp * motion * std::sin(2.0 * kPi * freq * t));
        }
    }
    return s;
}

// --- scenarios ---

// One spatialized harmonic-stack voice crossing the listener's field
// left -> center -> right at z = -4: pan, inverse-square rolloff, air
// absorption, proximity boost, and authored width collapse all move over the
// pass and land in the peak envelope.
TEST(BaselineRegression, FlybyPanAttenuation) {
    EngineSession session(MakeNullConfig(2, 0, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> stack = MakeHarmonicStack(
        static_cast<std::size_t>(2.0 * kRate), 220.0, 0.35f);
    HARKENTEST_ASSERT_NON_SILENT(stack);
    const VnPCMDesc pcm = MonoDesc(stack, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    // Exercise every distance-driven feature, including an explicit width
    // curve; disabled width curves now keep a point source at all distances.
    VnAttenuationDesc profile{};
    profile.structSize = sizeof(VnAttenuationDesc);
    profile.rolloff = VN_ROLLOFF_INVERSE_SQUARE;
    profile.minDistance = 1.0f;
    profile.maxDistance = 40.0f;
    profile.airAbsorptionNearHz = 18000.0f;
    profile.airAbsorptionFarHz = 900.0f;
    profile.proximityRadius = 6.0f;
    profile.proximityMaxBoostDb = 6.0f;
    profile.widthCurve.enabled = 1;
    profile.widthCurve.pointCount = 2;
    profile.widthCurve.points[0] = {profile.minDistance, 1.0f};
    profile.widthCurve.points[1] = {profile.maxDistance, 0.0f};
    profile.widthCurve.handles[0] = harkentest::EndpointHandles(
        profile.widthCurve.points[0], profile.widthCurve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(kFlybyProfile, &profile));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), -20.0f, 0.0f, -4.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, kFlybyProfile));

    const std::uint32_t blocks = ScenarioBlockCount(engine);
    std::vector<float> peaks;
    peaks.reserve(blocks * 2);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        const float progress =
            blocks > 1 ? static_cast<float>(b) /
                             static_cast<float>(blocks - 1)
                       : 0.0f;
        const float x = -20.0f + 40.0f * progress;
        ASSERT_EQ(VN_OK, Vn_SetVoicePosition(handle, Vec3(x, 0.0f, -4.0f)));
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpBlockPeaks(engine, &peaks);
    }
    CompareOrRecord("flyby_pan_attenuation", peaks);
}

// One 2D noise voice on the master bus: snap to 0 dB, ramp to -18 dB
// and back, then a fade-stop rendered through to silence — the full
// bus-gain and voice-fade arc in one envelope.
TEST(BaselineRegression, BusRampFadeStop) {
    EngineSession session(MakeNullConfig(2, 0, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> noise =
        MakeSeededNoise(static_cast<std::size_t>(2.0 * kRate), 0.25f);
    HARKENTEST_ASSERT_NON_SILENT(noise);
    const VnPCMDesc pcm = MonoDesc(noise, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    // First set on a bus snaps regardless of rampMs; spend it on 0 dB
    // so the scripted ramps below really ramp.
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    const std::uint32_t blocks = ScenarioBlockCount(engine);
    // Block-indexed schedule, proportional to the run: dip at ~0.25 s,
    // recover at ~0.75 s, fade-stop at ~1.1 s; the 200 ms fade ends
    // well before the run does, so the tail records the silent floor.
    const std::uint32_t rampDownBlock = blocks / 6;
    const std::uint32_t rampBackBlock = blocks / 2;
    const std::uint32_t stopBlock = (blocks * 3) / 4;
    std::vector<float> peaks;
    peaks.reserve(blocks * 2);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        if (b == rampDownBlock) {
            ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -18.0f, 300));
        }
        if (b == rampBackBlock) {
            ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 300));
        }
        if (b == stopBlock) {
            ASSERT_EQ(VN_OK, Vn_Stop(handle, 200));
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpBlockPeaks(engine, &peaks);
    }
    CompareOrRecord("bus_ramp_fade_stop", peaks);
}

// One 2D noise voice whose lowpass cutoff a Variable sweeps through a
// linear curve (0 -> 500 Hz, 1 -> 20 kHz) in a triangle 1 -> 0 -> 1:
// broadband peaks sag toward the closed cutoff and recover. Binding
// assembly uses the engine-internal surface, like the analytic sweep
// golden case — the public API drives the Variable only.
TEST(BaselineRegression, VariableLowpassSweep) {
    EngineSession session(MakeNullConfig(2, 0, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(kCutoffVariable,
                                         harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(
                  engine, &definition, 1).status);

    const std::vector<float> noise =
        MakeSeededNoise(static_cast<std::size_t>(2.0 * kRate), 0.25f);
    HARKENTEST_ASSERT_NON_SILENT(noise);
    const VnPCMDesc pcm = MonoDesc(noise, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    harken::Curve curve;
    const harken::CurvePoint pts[] = {{0.0f, 500.0f}, {1.0f, 20000.0f}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
    ASSERT_TRUE(engine->params.BindVoiceTarget(kCutoffVariable,
        harkentest::RenderHandle(engine, handle),
        harken::kTargetVoiceLowpassCutoff, curve));
    // Preserve the recorded main-branch audio positions independently of
    // how many updates a Variable write needs before it reaches Applied.
    constexpr std::uint64_t kInitialDrainBlock = 7;
    constexpr std::uint64_t kFirstCaptureBlock = 18;
    constexpr std::uint64_t kCaptureStrideBlocks = 10;
    const auto setAndAdvanceTo = [&](float value, std::uint64_t targetBlock) {
        ASSERT_LE(engine->mixer.BlocksRendered(), targetBlock);
        ASSERT_EQ(0u, engine->variables.PendingCount());
        const auto previousSequence = engine->logicCommits.Current();
        const auto previousApplied = engine->variables.AppliedCount();
        VnVariableTicketQueryDescV1 query{};
        query.structSize = sizeof(query);
        query.ticket.worldGeneration = engine->variables.CurrentWorldGeneration();
        query.ticket.transportGeneration = engine->variables.CurrentTransportEpoch();
        ASSERT_EQ(VN_OK, Vn_SetValue(kCutoffVariable, value));
        query.ticket.sequence = engine->logicCommits.Current();
        ASSERT_GT(query.ticket.sequence, previousSequence);
        for (std::uint64_t block = engine->mixer.BlocksRendered();
             block < targetBlock; ++block) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(targetBlock, engine->mixer.BlocksRendered());
        VnVariableTicketQueryResultV1 result{};
        result.structSize = sizeof(result);
        ASSERT_EQ(VN_OK, Vn_QueryVariableTicket(&query, &result));
        ASSERT_EQ(VN_VARIABLE_TICKET_FOUND, result.status);
        ASSERT_EQ(VN_VARIABLE_OUTCOME_APPLIED, result.outcome);
        ASSERT_EQ(previousApplied + 1u, engine->variables.AppliedCount());
        ASSERT_EQ(0u, engine->variables.PendingCount());
        float currentValue = 0.0f;
        ASSERT_TRUE(engine->variables.CurrentValue(kCutoffVariable, &currentValue));
        ASSERT_FLOAT_EQ(value, currentValue);
    };
    ASSERT_NO_FATAL_FAILURE(setAndAdvanceTo(1.0f, kInitialDrainBlock));

    const std::uint32_t blocks = ScenarioBlockCount(engine);
    std::vector<float> peaks;
    peaks.reserve(blocks * 2);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        // Triangle sweep across the run; the final block divides by
        // blocks - 1 so the sweep lands exactly back on 1.0.
        const double p =
            blocks > 1 ? static_cast<double>(b) /
                             static_cast<double>(blocks - 1)
                       : 0.0;
        const float value = static_cast<float>(std::fabs(1.0 - 2.0 * p));
        ASSERT_NO_FATAL_FAILURE(setAndAdvanceTo(
            value, kFirstCaptureBlock - 1u + kCaptureStrideBlocks * b));
        PumpBlockPeaks(engine, &peaks);
    }
    CompareOrRecord("variable_lowpass_sweep", peaks);
}

TEST(BaselineRegression, StreamedSubregionLoop) {
    EngineSession session(SceneNullConfig(1, 1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->streams.SetMaxServeFramesForTest(2048);

    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(8201);
    spec.sampleRate = 48000;
    spec.channelCount = 1;
    spec.chunkFrames = 128;
    spec.loopable = true;
    spec.loopStartFrame = 4096;
    spec.loopEndFrame = 8192;
    const std::vector<float> source = MakeDistinctSignal(24000);
    HARKENTEST_ASSERT_NON_SILENT(source);
    const std::string path = TempPath("baseline_streamed_subregion.stream");
    harkentest::WriteFileBytes(path,
                              harkentest::BuildStreamBytes(spec, source));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(spec.soundId, path.c_str()));

    VnPlayDesc play = MakePlayDesc(spec.soundId);
    play.loopCount = 5;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    (void)handle;

    const std::uint32_t blocks = ScenarioBlockCount(engine);
    std::vector<float> peaks;
    peaks.reserve(blocks * 2);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        engine->streams.SyncServe();
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpBlockPeaks(engine, &peaks);
    }
    CompareOrRecord("streamed_subregion_loop", peaks);
}

TEST(BaselineRegression, NonCommutingEffectChain) {
    EngineSession session(SceneNullConfig(2));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> noise =
        MakeSeededNoise(static_cast<std::size_t>(2.0 * kRate), 0.45f);
    HARKENTEST_ASSERT_NON_SILENT(noise);
    const VnPCMDesc pcm = MonoDesc(noise, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), -6.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const VnFeedbackDelayDesc delay = DelayDesc(25.0f, 0.50f, 0.70f, 1.0f);
    const VnThreeBandToneDesc tone =
        ToneDesc(7.0f, 220.0f, -5.0f, 1200.0f, 0.9f, 4.0f, 6000.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0,
                                       VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&delay)));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 1,
                                       VN_EFFECT_THREE_BAND_TONE,
                                       AsBase(&tone)));

    const std::uint32_t blocks = ScenarioBlockCount(engine);
    std::vector<float> peaks;
    peaks.reserve(blocks * 2);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpBlockPeaks(engine, &peaks);
    }
    CompareOrRecord("noncommuting_effect_chain", peaks);
}

TEST(BaselineRegression, CombinedM2Scene) {
    EngineSession session(SceneNullConfig(1, 1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->streams.SetMaxServeFramesForTest(2048);

    StreamSpec musicSpec;
    musicSpec.soundId = harkentest::ObjectId(8301);
    musicSpec.sampleRate = 48000;
    musicSpec.channelCount = 1;
    musicSpec.chunkFrames = 128;
    const std::vector<float> music =
        MakeMiniMusic(static_cast<std::size_t>(2.0 * kRate));
    HARKENTEST_ASSERT_NON_SILENT(music);
    const std::string musicPath = TempPath("baseline_m2_music.stream");
    harkentest::WriteFileBytes(
        musicPath, harkentest::BuildStreamBytes(musicSpec, music));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(musicSpec.soundId,
                                           musicPath.c_str()));

    const std::vector<float> ambience =
        MakeMiniAmbience(static_cast<std::size_t>(0.5 * kRate));
    HARKENTEST_ASSERT_NON_SILENT(ambience);
    const VnPCMDesc ambiencePcm = MonoDesc(ambience, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(8302), &ambiencePcm));

    const std::vector<float> oneShot =
        MakeMiniOneShot(static_cast<std::size_t>(0.25 * kRate));
    HARKENTEST_ASSERT_NON_SILENT(oneShot);
    const VnPCMDesc oneShotPcm = MonoDesc(oneShot, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(8303), &oneShotPcm));

    VnPlayDesc musicPlay = MakePlayDesc(musicSpec.soundId, -9.0f);
    VnVoiceHandle musicVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&musicPlay, &musicVoice));
    (void)musicVoice;

    VnPlayDesc ambiencePlay = MakePlayDesc(harkentest::ObjectId(8302), -13.0f);
    ambiencePlay.loopCount = 0;
    VnVoiceHandle ambienceVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&ambiencePlay, &ambienceVoice));

    const std::uint32_t blocks = ScenarioBlockCount(engine, 2.6);
    const std::uint32_t oneShotBlock = blocks / 3;
    const std::uint32_t fadeBlock = (blocks * 3) / 4;
    std::vector<float> peaks;
    peaks.reserve(blocks * 2);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        if (b == oneShotBlock) {
            VnPlayDesc oneShotPlay = MakePlayDesc(harkentest::ObjectId(8303), -1.0f);
            VnVoiceHandle oneShotVoice = 0;
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&oneShotPlay, &oneShotVoice));
            const VnFeedbackDelayDesc delay =
                DelayDesc(90.0f, 0.45f, 0.85f, 1.0f);
            ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(oneShotVoice, 0,
                                               VN_EFFECT_FEEDBACK_DELAY,
                                               AsBase(&delay)));
        }
        if (b == fadeBlock) {
            ASSERT_EQ(VN_OK, Vn_Stop(ambienceVoice, 400));
        }
        engine->streams.SyncServe();
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpBlockPeaks(engine, &peaks);
    }
    CompareOrRecord("combined_m2_scene", peaks);
}

// Priority-driven steal: a full two-voice pool under steal-quietest admits a
// higher-priority voice by stealing the quietest — a deterministic,
// metric-driven outcome. The per-policy victim selection, declick bound, and
// deferred-start bound are pinned exhaustively in the steal unit tests; this
// case freezes the composed steal's rendered envelope, which changes timbre at
// the steal block as the quiet voice gives way to the incoming one.
TEST(BaselineRegression, PriorityStealScene) {
    VnConfig config = SceneNullConfig(1, 0);
    config.maxVoices = 2; // a small pool so the third play must steal
    config.voiceStealPolicy = 1; // steal-quietest
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::size_t frames = static_cast<std::size_t>(2.0 * kRate);
    const std::vector<float> loud = MakeHarmonicStack(frames, 180.0, 0.45f);
    const std::vector<float> quiet = MakeHarmonicStack(frames, 330.0, 0.45f);
    const std::vector<float> incoming = MakeHarmonicStack(frames, 90.0, 0.45f);
    const VnPCMDesc loudPcm = MonoDesc(loud, 48000);
    const VnPCMDesc quietPcm = MonoDesc(quiet, 48000);
    const VnPCMDesc incomingPcm = MonoDesc(incoming, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &loudPcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &quietPcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(3), &incomingPcm));

    VnPlayDesc a = MakePlayDesc(harkentest::ObjectId(1), -3.0f);
    a.loopCount = 0;
    a.priority = 128;
    VnVoiceHandle ha = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&a, &ha));
    VnPlayDesc b = MakePlayDesc(harkentest::ObjectId(2), -24.0f); // the quietest -> the steal victim
    b.loopCount = 0;
    b.priority = 128;
    VnVoiceHandle hb = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&b, &hb));

    const std::uint32_t blocks = ScenarioBlockCount(engine);
    const std::uint32_t stealBlock = blocks / 2;
    std::vector<float> peaks;
    peaks.reserve(blocks * 2);
    for (std::uint32_t bl = 0; bl < blocks; ++bl) {
        if (bl == stealBlock) {
            VnPlayDesc c = MakePlayDesc(harkentest::ObjectId(3), -3.0f);
            c.loopCount = 0;
            c.priority = 200; // higher priority admits by stealing the quietest
            VnVoiceHandle hc = 0;
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&c, &hc));
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpBlockPeaks(engine, &peaks);
    }
    CompareOrRecord("priority_steal_scene", peaks);
}

// Audibility parking: an infinite-loop voice driven inaudible parks (its render
// slot frees), virtual time advances while parked, and raising it audible again
// returns it to resume rendering. The park/return mechanics (slot-freed proof,
// hysteresis, virtual-time advance, returned-vs-fresh match) are pinned in the
// park unit tests; this case freezes the composed cycle's envelope — the
// audible -> parked-silent -> returned-audible arc.
TEST(BaselineRegression, ParkReturnScene) {
    VnConfig config = SceneNullConfig(1, 0);
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    config.maxParkedVoices = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> tone = MakeHarmonicStack(
        static_cast<std::size_t>(0.5 * kRate), 200.0, 0.4f);
    const VnPCMDesc pcm = MonoDesc(tone, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    play.loopCount = 0; // infinite: park-eligible while inaudible
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    const std::uint32_t blocks = ScenarioBlockCount(engine, 2.0);
    const std::uint32_t parkBlock = blocks / 4;
    const std::uint32_t returnBlock = (blocks * 3) / 4;
    std::vector<float> peaks;
    peaks.reserve(blocks * 2);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        if (b == parkBlock) {
            // Drop it well below the park threshold: it goes inaudible and parks.
            ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, -80.0f, 0));
        }
        if (b == returnBlock) {
            // Raise it back above the hysteresis band: it returns and resumes.
            ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 0.0f, 0));
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpBlockPeaks(engine, &peaks);
    }
    CompareOrRecord("park_return_scene", peaks);
}

// Bus graph composition: a voice into submix A, an additive A->B send, a bus
// effect on B, and B's fader split around its BusVolume, all evaluated in
// published plan order (A and B finalized before the master). Send, effect, and
// fader mechanics are pinned in the bus unit tests; this case freezes the
// composed graph's rendered envelope.
TEST(BaselineRegression, BusGraphScene) {
    VnConfig config = SceneNullConfig(2, 0); // fx slots also cover the bus chain
    config.fxSlotsPerBus = 4;
    config.busSendSlotsPerBus = 4;
    config.topologyPlanPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> noise =
        MakeSeededNoise(static_cast<std::size_t>(2.0 * kRate), 0.35f);
    HARKENTEST_ASSERT_NON_SILENT(noise);
    const VnPCMDesc pcm = MonoDesc(noise, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    constexpr VnBusId kA = harkentest::ObjectId(20);
    constexpr VnBusId kB = harkentest::ObjectId(21);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kA, kB, -6.0f)); // A -> B at -6 dB
    const VnFeedbackDelayDesc bDelay = DelayDesc(40.0f, 0.35f, 0.6f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(kB, 0, VN_EFFECT_FEEDBACK_DELAY,
                                     AsBase(&bDelay)));
    ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kB, 0)); // the delay runs post-fader
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kB, -3.0f, 0));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), -6.0f);
    play.loopCount = 0;
    play.busId = kA;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    const std::uint32_t blocks = ScenarioBlockCount(engine);
    std::vector<float> peaks;
    peaks.reserve(blocks * 2);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpBlockPeaks(engine, &peaks);
    }
    CompareOrRecord("bus_graph_scene", peaks);
}

// --- multichannel scenarios (spec M5 §11.2/§11.3, recorded regression) ---
// These three freeze the S1 render-contract amendment's three new surfaces as
// full end-to-end envelopes, complementing the analytic per-channel goldens:
// the widened N-channel mix + pan, the master->device output fold, and the
// per-bus (non-master) fold with a custom matrix. Every device channel is
// recorded (PumpBlockPeaks capture-all), so a regression on any layout channel
// — including a surround or LFE plane the stereo baselines cannot see — shows
// up as a peak-envelope diff. The nine stereo baselines above are untouched.

// 5.1 mix, device 5.1 (master does not fold — six channels captured): a mono
// point source sweeps the front arc (FL->FC->FR), a six-channel 5.1 bed fills
// every plane including the surrounds and LFE, and a mono voice carries a
// non-commuting effect chain — multi-part content, an effect, and a pan sweep
// composed across the full mix width.
TEST(BaselineRegression, MultichannelFiveOnePanBed) {
    EngineSession session(SceneNullConfigLayout(VN_CHANNEL_LAYOUT_5_1,
                                                VN_CHANNEL_LAYOUT_5_1, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::size_t frames = static_cast<std::size_t>(2.0 * kRate);
    const std::vector<float> stack = MakeHarmonicStack(frames, 220.0, 0.35f);
    HARKENTEST_ASSERT_NON_SILENT(stack);
    const VnPCMDesc stackPcm = MonoDesc(stack, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &stackPcm));

    const std::vector<float> bed = MakeChannelBed(frames, 6);
    HARKENTEST_ASSERT_NON_SILENT(bed);
    const VnPCMDesc bedPcm = InterleavedDesc(bed, 6, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &bedPcm));

    const std::vector<float> noise = MakeSeededNoise(frames, 0.30f);
    const VnPCMDesc noisePcm = MonoDesc(noise, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(3), &noisePcm));

    // The panning point source, entering front-left.
    VnPlayDesc panPlay = MakePlayDesc3D(harkentest::ObjectId(1), -20.0f, 0.0f, -4.0f);
    panPlay.loopCount = 0;
    VnVoiceHandle panVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&panPlay, &panVoice));

    // The full-width 5.1 bed (2D multichannel source, channel-for-channel).
    VnPlayDesc bedPlay = MakePlayDesc(harkentest::ObjectId(2), -6.0f);
    bedPlay.loopCount = 0;
    VnVoiceHandle bedVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&bedPlay, &bedVoice));
    (void)bedVoice;

    // A mono voice through a non-commuting effect chain.
    VnPlayDesc fxPlay = MakePlayDesc(harkentest::ObjectId(3), -6.0f);
    fxPlay.loopCount = 0;
    VnVoiceHandle fxVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&fxPlay, &fxVoice));
    const VnFeedbackDelayDesc delay = DelayDesc(25.0f, 0.50f, 0.70f, 1.0f);
    const VnThreeBandToneDesc tone =
        ToneDesc(7.0f, 220.0f, -5.0f, 1200.0f, 0.9f, 4.0f, 6000.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(fxVoice, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&delay)));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(fxVoice, 1, VN_EFFECT_THREE_BAND_TONE,
                                       AsBase(&tone)));

    const std::uint32_t blocks = ScenarioBlockCount(engine);
    std::vector<float> peaks;
    peaks.reserve(blocks * 6);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        const float progress =
            blocks > 1 ? static_cast<float>(b) /
                             static_cast<float>(blocks - 1)
                       : 0.0f;
        const float x = -20.0f + 40.0f * progress;
        ASSERT_EQ(VN_OK, Vn_SetVoicePosition(panVoice, Vec3(x, 0.0f, -4.0f)));
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpBlockPeaks(engine, &peaks, 0);
    }
    CompareOrRecordMulti("multichannel_5_1_pan_bed", peaks, 6);
}

// 7.1 mix, device stereo (the master->device output fold — two channels
// captured): a front-arc pan sweep and a full eight-channel 7.1 bed, including
// the side and back surrounds, fold to stereo through the master's default
// matrix (ITU-R BS.775 fronts + the pinned engineering extension for the 7.1
// surrounds). Freezes the master output downmix on a live multi-voice scene.
TEST(BaselineRegression, MultichannelSevenOneToStereoFold) {
    EngineSession session(SceneNullConfigLayout(VN_CHANNEL_LAYOUT_7_1,
                                                VN_CHANNEL_LAYOUT_STEREO, 1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::size_t frames = static_cast<std::size_t>(2.0 * kRate);
    const std::vector<float> stack = MakeHarmonicStack(frames, 180.0, 0.35f);
    HARKENTEST_ASSERT_NON_SILENT(stack);
    const VnPCMDesc stackPcm = MonoDesc(stack, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &stackPcm));

    const std::vector<float> bed = MakeChannelBed(frames, 8);
    HARKENTEST_ASSERT_NON_SILENT(bed);
    const VnPCMDesc bedPcm = InterleavedDesc(bed, 8, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &bedPcm));

    VnPlayDesc panPlay = MakePlayDesc3D(harkentest::ObjectId(1), -20.0f, 0.0f, -4.0f);
    panPlay.loopCount = 0;
    VnVoiceHandle panVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&panPlay, &panVoice));

    VnPlayDesc bedPlay = MakePlayDesc(harkentest::ObjectId(2), -6.0f);
    bedPlay.loopCount = 0;
    VnVoiceHandle bedVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&bedPlay, &bedVoice));
    (void)bedVoice;

    const std::uint32_t blocks = ScenarioBlockCount(engine);
    std::vector<float> peaks;
    peaks.reserve(blocks * 2);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        const float progress =
            blocks > 1 ? static_cast<float>(b) /
                             static_cast<float>(blocks - 1)
                       : 0.0f;
        const float x = -20.0f + 40.0f * progress;
        ASSERT_EQ(VN_OK, Vn_SetVoicePosition(panVoice, Vec3(x, 0.0f, -4.0f)));
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpBlockPeaks(engine, &peaks, 0);
    }
    CompareOrRecordMulti("multichannel_7_1_to_stereo_fold", peaks, 2);
}

// 5.1 mix, device 5.1 (master does not fold — six channels captured), with a
// sub-bus set to a STEREO output layout carrying a custom, clearly non-default
// downmix matrix. A mono 2D voice into the sub-bus folds through that matrix
// and name-places (L->FL, R->FR) into the captured master fronts; a direct
// master 5.1 bed fills FC / LFE / the surrounds, proving the per-bus fold is
// isolated to the fronts and the master itself does not fold. At the midpoint
// the matrix is reset to the ITU default, so the envelope pins both the custom
// matrix and the reset path.
TEST(BaselineRegression, MultichannelFiveOnePerBusCustomFold) {
    EngineSession session(SceneNullConfigLayout(VN_CHANNEL_LAYOUT_5_1,
                                                VN_CHANNEL_LAYOUT_5_1, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::size_t frames = static_cast<std::size_t>(2.0 * kRate);
    const std::vector<float> noise = MakeSeededNoise(frames, 0.30f);
    HARKENTEST_ASSERT_NON_SILENT(noise);
    const VnPCMDesc noisePcm = MonoDesc(noise, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &noisePcm));

    const std::vector<float> bed = MakeChannelBed(frames, 6);
    HARKENTEST_ASSERT_NON_SILENT(bed);
    const VnPCMDesc bedPcm = InterleavedDesc(bed, 6, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &bedPcm));

    // A sub-bus that folds its 5.1 render to a stereo output through a custom
    // asymmetric matrix (Lo = 1.5*FL, Ro = 0.5*FR; every other coefficient 0).
    constexpr VnBusId kSubBus = harkentest::ObjectId(30);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSubBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusOutputLayout(kSubBus, VN_CHANNEL_LAYOUT_STEREO));
    VnDownmixMatrixDesc custom{};
    custom.structSize = sizeof(VnDownmixMatrixDesc);
    custom.srcLayout = VN_CHANNEL_LAYOUT_5_1;
    custom.dstLayout = VN_CHANNEL_LAYOUT_STEREO;
    custom.coeffs[0 * 8 + 0] = 1.5f; // dst Lo (row 0) from src FL (col 0)
    custom.coeffs[1 * 8 + 1] = 0.5f; // dst Ro (row 1) from src FR (col 1)
    ASSERT_EQ(VN_OK, Vn_SetBusDownmixMatrix(kSubBus, &custom));

    // A mono 2D voice into the sub-bus lands on the front L/R pair, then folds.
    VnPlayDesc subPlay = MakePlayDesc(harkentest::ObjectId(1), -6.0f);
    subPlay.loopCount = 0;
    subPlay.busId = kSubBus;
    VnVoiceHandle subVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&subPlay, &subVoice));
    (void)subVoice;

    // A direct master 5.1 bed (does not touch the sub-bus fold).
    VnPlayDesc bedPlay = MakePlayDesc(harkentest::ObjectId(2), -9.0f);
    bedPlay.loopCount = 0;
    VnVoiceHandle bedVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&bedPlay, &bedVoice));
    (void)bedVoice;

    const std::uint32_t blocks = ScenarioBlockCount(engine);
    const std::uint32_t resetBlock = blocks / 2;
    std::vector<float> peaks;
    peaks.reserve(blocks * 6);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        if (b == resetBlock) {
            // Revert to the ITU-R BS.775 default 5.1->stereo fold.
            ASSERT_EQ(VN_OK, Vn_ResetBusDownmixMatrix(kSubBus));
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpBlockPeaks(engine, &peaks, 0);
    }
    CompareOrRecordMulti("multichannel_5_1_perbus_custom_fold", peaks, 6);
}

} // namespace
