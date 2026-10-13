// Sample-exact characterization of the two released insert-effect kernels
// (feedback delay, three-band tone), driven at the InsertEffect seam over a
// deterministic input and frozen as full-output golden files. The point is a
// byte-exact record of what the shipping kernels emit: any change in the delay
// recurrence, the soft-saturation, the RBJ coefficient derivation, or the
// biquad state update shows up as a golden mismatch, not a silent behavior
// drift. Kernels are exercised directly through the effect pool (Acquire ->
// StateFor -> BuildEffect), so no engine host is involved.
//
// Golden format (`<name>_<toolchain>.f32kernel`, binary, native byte order —
// the only order this project targets): uint32 magic, uint32 channelCount,
// uint32 frameCount, then channelCount * frameCount float32 samples in planar
// order (all of channel 0, then channel 1). Comparison is bit-exact (tolerance
// zero). Recording mode: run with HARKEN_WRITE_BASELINES=1 to (re)write the
// files; default mode requires them to exist. Every golden change lands with a
// same-commit entry in the baselines CHANGELOG.
//
// Per-toolchain goldens: the driving input is built from integer selection and
// a power-of-two divide, so it is identical on every toolchain; the kernel
// math (tanh, RBJ transcendentals) is not, and the characterization contract
// is same-toolchain byte-exact preservation. Each build channel therefore
// records and compares its own file, named by a compile-time toolchain suffix
// (two files per kernel). A golden is authoritative only for the toolchain
// that recorded it; the suffix keeps the channels from cross-reading. clang-cl
// defines both _MSC_VER and __clang__, so the suffix discriminates on
// __clang__ alone: any Clang front end (including clang-cl) is "clang", a pure
// MSVC front end is "msvc".
#include <harken/Harken.h>

#include "harken/render/BuiltinEffects.h"
#include "harken/render/EffectPool.h"
#include "harken/render/InsertEffect.h"
#include "harken/core/EngineConfig.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#ifndef HARKEN_BASELINE_DIR
#error "HARKEN_BASELINE_DIR must point at HarkenEngine/Test/Baselines"
#endif

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kBlocks = 8;
constexpr std::uint32_t kFrames = kBlockFrames * kBlocks;
constexpr std::uint32_t kGoldenMagic = 0x314E564Bu;

// Compile-time toolchain discriminator for the golden filename. clang-cl
// defines both _MSC_VER and __clang__, so any Clang front end resolves to
// "clang" and only a pure MSVC front end resolves to "msvc".
#if defined(__clang__)
constexpr const char* kToolchainSuffix = "clang";
#else
constexpr const char* kToolchainSuffix = "msvc";
#endif

// One scheduled parameter step: apply `value` to `paramIndex` at the boundary
// before block `block`.
struct ParamStep {
    std::uint32_t block;
    std::uint32_t paramIndex;
    float value;
};

// Deterministic input: a unit impulse on frame 0 of every channel plus a fixed
// pseudo-random tail. Each random sample is an integer selection divided by a
// power of two, so every value is exactly representable and identical across
// toolchains — only the kernel under test can move the output.
std::vector<std::vector<float>> MakeInput() {
    std::vector<std::vector<float>> in(
        kChannels, std::vector<float>(kFrames, 0.0f));
    std::uint32_t rng = 0x1234ABCDu; // fixed seed
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < kChannels; ++c) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            const std::int32_t bits =
                static_cast<std::int32_t>(rng & 0xFFFFu) - 32768;
            in[c][f] = static_cast<float>(bits) / 131072.0f; // /2^17 -> [-0.25,0.25)
        }
    }
    in[0][0] = 1.0f;
    in[1][0] = 1.0f;
    return in;
}

// Runs a built effect over the deterministic input for kBlocks blocks,
// applying each scheduled step at its block boundary, and returns the full
// planar float output (channel 0 frames, then channel 1 frames).
std::vector<float> RunInput(const harken::InsertEffect& fx, void* state,
                            const std::vector<ParamStep>& schedule) {
    std::vector<std::vector<float>> work = MakeInput();
    std::vector<float> out(static_cast<std::size_t>(kChannels) * kFrames, 0.0f);
    for (std::uint32_t b = 0; b < kBlocks; ++b) {
        for (const ParamStep& step : schedule) {
            if (step.block == b) {
                fx.setParam(state, step.paramIndex, step.value);
            }
        }
        const std::uint32_t base = b * kBlockFrames;
        float* chans[kChannels];
        for (std::uint32_t c = 0; c < kChannels; ++c) {
            chans[c] = work[c].data() + base;
        }
        fx.process(state, chans, kChannels, 0, kBlockFrames);
        for (std::uint32_t c = 0; c < kChannels; ++c) {
            for (std::uint32_t i = 0; i < kBlockFrames; ++i) {
                out[static_cast<std::size_t>(c) * kFrames + base + i] =
                    work[c][base + i];
            }
        }
    }
    return out;
}

VnFeedbackDelayDesc DelayDesc() {
    VnFeedbackDelayDesc d{};
    d.structSize = sizeof(VnFeedbackDelayDesc);
    d.delayMs = 10.0f;
    d.feedback = 0.5f;
    d.wet = 1.0f;
    d.dry = 1.0f;
    return d;
}

VnThreeBandToneDesc ToneDesc() {
    VnThreeBandToneDesc t{};
    t.structSize = sizeof(VnThreeBandToneDesc);
    t.lowShelfGainDb = 6.0f;
    t.lowShelfFreqHz = 200.0f;
    t.peakGainDb = -4.0f;
    t.peakFreqHz = 1000.0f;
    t.peakQ = 1.5f;
    t.highShelfGainDb = 3.0f;
    t.highShelfFreqHz = 6000.0f;
    return t;
}

harken::EngineConfig KernelConfig() {
    VnConfig publicConfig{};
    publicConfig.structSize = sizeof(VnConfig);
    publicConfig.deviceMode = VN_DEVICE_NULL;
    publicConfig.sampleRate = kRate;
    harken::EngineConfig config;
    if (harken::ResolveConfig(&publicConfig, &config) != VN_OK) {
        std::abort();
    }
    return config;
}

// --- golden file plumbing ---

std::string GoldenPath(const char* name) {
    return std::string(HARKEN_BASELINE_DIR "/") + name + "_" +
           kToolchainSuffix + ".f32kernel";
}

bool WriteBaselinesRequested() {
    const char* env = std::getenv("HARKEN_WRITE_BASELINES");
    return env != nullptr && env[0] == '1' && env[1] == '\0';
}

bool WriteGolden(const std::string& path, const std::vector<float>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    const std::uint32_t header[3] = {kGoldenMagic, kChannels, kFrames};
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(float)));
    return out.good();
}

// Reads a golden and validates its magic and dimensions. Returns false on any
// structural mismatch or a non-finite payload so the compare path reports it
// with the record hint.
bool ReadGolden(const std::string& path, std::vector<float>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::uint32_t header[3] = {0, 0, 0};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in.good() || header[0] != kGoldenMagic || header[1] != kChannels ||
        header[2] != kFrames) {
        return false;
    }
    out->resize(static_cast<std::size_t>(kChannels) * kFrames);
    in.read(reinterpret_cast<char*>(out->data()),
            static_cast<std::streamsize>(out->size() * sizeof(float)));
    if (in.gcount() !=
        static_cast<std::streamsize>(out->size() * sizeof(float))) {
        return false;
    }
    for (const float value : *out) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return in.peek() == std::ifstream::traits_type::eof();
}

// Records or bit-exactly verifies one kernel golden, per the mode.
void CompareOrRecordGolden(const char* name, const std::vector<float>& data) {
    // A broken kernel that emits NaN/Inf must never be recorded nor pass.
    for (std::size_t i = 0; i < data.size(); ++i) {
        ASSERT_TRUE(std::isfinite(data[i]))
            << "kernel '" << name << "' produced a non-finite sample at index "
            << i;
    }
    // Guard against an all-silent capture masquerading as a valid golden.
    float peak = 0.0f;
    for (const float v : data) {
        peak = std::fmax(peak, std::fabs(v));
    }
    ASSERT_GT(peak, 1e-6f) << "kernel '" << name << "' rendered silent";

    const std::string path = GoldenPath(name);
    if (WriteBaselinesRequested()) {
        ASSERT_TRUE(WriteGolden(path, data))
            << "cannot write golden file: " << path;
        std::printf("golden written: %s\n", path.c_str());
        return;
    }

    std::vector<float> expected;
    ASSERT_TRUE(ReadGolden(path, &expected))
        << "golden missing, unreadable, or shape-mismatched: " << path
        << " — record it by rerunning with HARKEN_WRITE_BASELINES=1";
    ASSERT_EQ(expected.size(), data.size());
    // Bit-exact compare: reinterpret each float as its bit pattern so a change
    // in the last mantissa bit is caught (tolerance zero).
    for (std::size_t i = 0; i < data.size(); ++i) {
        std::uint32_t want = 0;
        std::uint32_t got = 0;
        std::memcpy(&want, &expected[i], sizeof(want));
        std::memcpy(&got, &data[i], sizeof(got));
        EXPECT_EQ(want, got)
            << "kernel '" << name << "' sample " << i << " (channel "
            << (i / kFrames) << ", frame " << (i % kFrames) << ") diverged: "
            << "expected " << expected[i] << ", got " << data[i];
    }
}

// --- feedback-delay characterization ---

TEST(DelayKernelCharacterization, FullOutputGolden) {
    harken::EngineConfig config = KernelConfig();
    harken::EffectPoolBank bank(config);
    const harken::EffectInstanceRef ref =
        bank.Acquire(VN_EFFECT_FEEDBACK_DELAY);
    ASSERT_EQ(VN_EFFECT_FEEDBACK_DELAY, ref.type);
    void* state = bank.StateFor(ref.type, ref.poolIndex);
    ASSERT_NE(nullptr, state);
    harken::InsertEffect fx = harken::BuildEffect(VN_EFFECT_FEEDBACK_DELAY, state);
    fx.reset(state);
    const VnFeedbackDelayDesc desc = DelayDesc();
    harken::ApplyEffectDesc(VN_EFFECT_FEEDBACK_DELAY, &desc, state);
    // Each of the four parameters stepped once, at its own block boundary, so
    // the golden covers mid-stream reconfiguration of every parameter.
    const std::vector<ParamStep> schedule = {
        {4, harken::kDelayParamFeedback, 0.7f},
        {5, harken::kDelayParamWet, 0.6f},
        {6, harken::kDelayParamDry, 0.4f},
        {7, harken::kDelayParamTimeMs, 15.0f},
    };
    const std::vector<float> out = RunInput(fx, state, schedule);
    bank.Recycle(ref);
    CompareOrRecordGolden("delay_kernel_characterization", out);
}

TEST(DelayKernelCharacterization, ResetThenReprocessEqualsFresh) {
    harken::EngineConfig config = KernelConfig();
    harken::EffectPoolBank bank(config);
    const harken::EffectInstanceRef ref =
        bank.Acquire(VN_EFFECT_FEEDBACK_DELAY);
    ASSERT_EQ(VN_EFFECT_FEEDBACK_DELAY, ref.type);
    void* state = bank.StateFor(ref.type, ref.poolIndex);
    ASSERT_NE(nullptr, state);
    harken::InsertEffect fx = harken::BuildEffect(VN_EFFECT_FEEDBACK_DELAY, state);
    const VnFeedbackDelayDesc desc = DelayDesc();

    fx.reset(state);
    harken::ApplyEffectDesc(VN_EFFECT_FEEDBACK_DELAY, &desc, state);
    const std::vector<float> fresh = RunInput(fx, state, {});

    // reset() restores defaults, so the fresh reconstruction re-applies the
    // descriptor exactly as the pool-acquire path does.
    fx.reset(state);
    harken::ApplyEffectDesc(VN_EFFECT_FEEDBACK_DELAY, &desc, state);
    const std::vector<float> reprocessed = RunInput(fx, state, {});
    bank.Recycle(ref);

    ASSERT_EQ(fresh.size(), reprocessed.size());
    EXPECT_EQ(fresh, reprocessed);
}

TEST(DelayKernelCharacterization, TailFramesForChosenParams) {
    harken::EngineConfig config = KernelConfig();
    harken::EffectPoolBank bank(config);
    const harken::EffectInstanceRef ref =
        bank.Acquire(VN_EFFECT_FEEDBACK_DELAY);
    ASSERT_EQ(VN_EFFECT_FEEDBACK_DELAY, ref.type);
    void* state = bank.StateFor(ref.type, ref.poolIndex);
    ASSERT_NE(nullptr, state);
    harken::InsertEffect fx = harken::BuildEffect(VN_EFFECT_FEEDBACK_DELAY, state);
    fx.reset(state);
    fx.setParam(state, harken::kDelayParamTimeMs, 10.0f); // 480 samples at 48 kHz
    fx.setParam(state, harken::kDelayParamFeedback, 0.5f);
    // 480 * (ceil(ln(1e-3)/ln(0.5)) + 1) = 480 * (10 + 1).
    EXPECT_EQ(5280u, fx.tailFrames(state));
    fx.setParam(state, harken::kDelayParamFeedback, 0.0f);
    // No feedback: a single delayed copy, exactly one delay period of tail.
    EXPECT_EQ(480u, fx.tailFrames(state));
    bank.Recycle(ref);
}

// --- three-band-tone characterization ---

TEST(ToneKernelCharacterization, FullOutputGolden) {
    harken::EngineConfig config = KernelConfig();
    harken::EffectPoolBank bank(config);
    const harken::EffectInstanceRef ref =
        bank.Acquire(VN_EFFECT_THREE_BAND_TONE);
    ASSERT_EQ(VN_EFFECT_THREE_BAND_TONE, ref.type);
    void* state = bank.StateFor(ref.type, ref.poolIndex);
    ASSERT_NE(nullptr, state);
    harken::InsertEffect fx =
        harken::BuildEffect(VN_EFFECT_THREE_BAND_TONE, state);
    fx.reset(state);
    const VnThreeBandToneDesc desc = ToneDesc();
    harken::ApplyEffectDesc(VN_EFFECT_THREE_BAND_TONE, &desc, state);
    // Each of the seven tone parameters stepped once, at its own block
    // boundary, to a value inside the validated range that visibly reshapes
    // the spectrum — so the golden pins every setParam index's routing, not
    // just the whole-descriptor application. A migrated dispatch that swapped
    // two indices would recompute the wrong band and diverge from this record.
    const std::vector<ParamStep> schedule = {
        {1, harken::kToneParamLowShelfGainDb, -10.0f},
        {2, harken::kToneParamLowShelfFreqHz, 400.0f},
        {3, harken::kToneParamPeakGainDb, 12.0f},
        {4, harken::kToneParamPeakFreqHz, 3000.0f},
        {5, harken::kToneParamPeakQ, 6.0f},
        {6, harken::kToneParamHighShelfGainDb, -8.0f},
        {7, harken::kToneParamHighShelfFreqHz, 9000.0f},
    };
    const std::vector<float> out = RunInput(fx, state, schedule);
    bank.Recycle(ref);
    CompareOrRecordGolden("tone_kernel_characterization", out);
}

TEST(ToneKernelCharacterization, ResetThenReprocessEqualsFresh) {
    harken::EngineConfig config = KernelConfig();
    harken::EffectPoolBank bank(config);
    const harken::EffectInstanceRef ref =
        bank.Acquire(VN_EFFECT_THREE_BAND_TONE);
    ASSERT_EQ(VN_EFFECT_THREE_BAND_TONE, ref.type);
    void* state = bank.StateFor(ref.type, ref.poolIndex);
    ASSERT_NE(nullptr, state);
    harken::InsertEffect fx =
        harken::BuildEffect(VN_EFFECT_THREE_BAND_TONE, state);
    const VnThreeBandToneDesc desc = ToneDesc();

    fx.reset(state);
    harken::ApplyEffectDesc(VN_EFFECT_THREE_BAND_TONE, &desc, state);
    const std::vector<float> fresh = RunInput(fx, state, {});

    fx.reset(state);
    harken::ApplyEffectDesc(VN_EFFECT_THREE_BAND_TONE, &desc, state);
    const std::vector<float> reprocessed = RunInput(fx, state, {});
    bank.Recycle(ref);

    ASSERT_EQ(fresh.size(), reprocessed.size());
    EXPECT_EQ(fresh, reprocessed);
}

TEST(ToneKernelCharacterization, ReportsConfiguredSectionTails) {
    harken::EngineConfig config = KernelConfig();
    harken::EffectPoolBank bank(config);
    const harken::EffectInstanceRef ref =
        bank.Acquire(VN_EFFECT_THREE_BAND_TONE);
    ASSERT_EQ(VN_EFFECT_THREE_BAND_TONE, ref.type);
    void* state = bank.StateFor(ref.type, ref.poolIndex);
    ASSERT_NE(nullptr, state);
    harken::InsertEffect fx =
        harken::BuildEffect(VN_EFFECT_THREE_BAND_TONE, state);
    fx.reset(state);
    // Neutral sections are exact identities and do not extend a voice.
    EXPECT_EQ(0u, fx.tailFrames(state));

    const VnThreeBandToneDesc desc = ToneDesc();
    harken::ApplyEffectDesc(VN_EFFECT_THREE_BAND_TONE, &desc, state);
    const auto* tone = static_cast<const harken::ToneEffectState*>(state);
    std::uint32_t expected = harken::SaturatingTailAdd(
        tone->lowShelfTailFrames, tone->peakTailFrames);
    expected = harken::SaturatingTailAdd(expected, tone->highShelfTailFrames);
    EXPECT_EQ(expected, fx.tailFrames(state));
    EXPECT_GT(expected, 0u);
    bank.Recycle(ref);
}

TEST(ToneKernelCharacterization, NeutralizedSectionRetainsTailUntilReset) {
    harken::EngineConfig config = KernelConfig();
    harken::EffectPoolBank bank(config);
    const harken::EffectInstanceRef ref =
        bank.Acquire(VN_EFFECT_THREE_BAND_TONE);
    ASSERT_EQ(VN_EFFECT_THREE_BAND_TONE, ref.type);
    void* state = bank.StateFor(ref.type, ref.poolIndex);
    ASSERT_NE(nullptr, state);
    harken::InsertEffect fx =
        harken::BuildEffect(VN_EFFECT_THREE_BAND_TONE, state);
    fx.reset(state);
    EXPECT_EQ(0u, fx.tailFrames(state));

    fx.setParam(state, harken::kToneParamLowShelfGainDb, 6.0f);
    float impulse[kBlockFrames] = {};
    impulse[0] = 1.0f;
    float* channels[] = {impulse};
    fx.process(state, channels, 1, 0, kBlockFrames);

    auto* tone = static_cast<harken::ToneEffectState*>(state);
    ASSERT_TRUE(tone->lowShelfState[0].z1 != 0.0f ||
                tone->lowShelfState[0].z2 != 0.0f);
    const harken::BiquadState retained = tone->lowShelfState[0];
    fx.setParam(state, harken::kToneParamLowShelfGainDb, 0.0f);
    EXPECT_EQ(retained.z1, tone->lowShelfState[0].z1);
    EXPECT_EQ(retained.z2, tone->lowShelfState[0].z2);
    ASSERT_GT(tone->lowShelfTailFrames, 0u);
    EXPECT_EQ(tone->lowShelfTailFrames, fx.tailFrames(state));

    fx.reset(state);
    EXPECT_EQ(0u, fx.tailFrames(state));
    EXPECT_EQ(0.0f, tone->lowShelfState[0].z1);
    EXPECT_EQ(0.0f, tone->lowShelfState[0].z2);
    bank.Recycle(ref);
}

// --- pool state-address stability ---
// A pooled instance's state pointer, resolved by (type, index), is a frozen
// contract: the audio side builds the InsertEffect over the same address the
// logic side reset/configured, so the address must be stable for the bank's
// life and survive a recycle/re-acquire of the slot.
TEST(EffectPoolAddressStability, StateAddressStableAcrossRecycle) {
    harken::EngineConfig config = KernelConfig();
    harken::EffectPoolBank bank(config);

    const harken::EffectInstanceRef delay =
        bank.Acquire(VN_EFFECT_FEEDBACK_DELAY);
    ASSERT_EQ(VN_EFFECT_FEEDBACK_DELAY, delay.type);
    const harken::EffectInstanceRef tone =
        bank.Acquire(VN_EFFECT_THREE_BAND_TONE);
    ASSERT_EQ(VN_EFFECT_THREE_BAND_TONE, tone.type);

    void* const delayAddr = bank.StateFor(delay.type, delay.poolIndex);
    void* const toneAddr = bank.StateFor(tone.type, tone.poolIndex);
    ASSERT_NE(nullptr, delayAddr);
    ASSERT_NE(nullptr, toneAddr);

    bank.Recycle(delay);
    bank.Recycle(tone);

    const harken::EffectInstanceRef delay2 =
        bank.Acquire(VN_EFFECT_FEEDBACK_DELAY);
    const harken::EffectInstanceRef tone2 =
        bank.Acquire(VN_EFFECT_THREE_BAND_TONE);
    // The LIFO freelist hands the just-recycled slot back, and its state
    // address is unchanged.
    EXPECT_EQ(delay.poolIndex, delay2.poolIndex);
    EXPECT_EQ(tone.poolIndex, tone2.poolIndex);
    EXPECT_EQ(delayAddr, bank.StateFor(delay2.type, delay2.poolIndex));
    EXPECT_EQ(toneAddr, bank.StateFor(tone2.type, tone2.poolIndex));

    // Resolution is a pure function of (type, index), independent of whether
    // the slot is currently acquired.
    EXPECT_EQ(delayAddr, bank.StateFor(VN_EFFECT_FEEDBACK_DELAY, delay.poolIndex));
    EXPECT_EQ(toneAddr, bank.StateFor(VN_EFFECT_THREE_BAND_TONE, tone.poolIndex));

    bank.Recycle(delay2);
    bank.Recycle(tone2);
}

} // namespace
