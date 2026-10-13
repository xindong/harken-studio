// Characterizes render-block timing while a feedback delay and high-Q biquad
// decay through the subnormal range. The two arms differ only in the pump
// thread's floating-point mode. Results are reported without a threshold.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Engine.h"
#include "Common/RawVoiceTestSupport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || \
    defined(__x86_64__)
#define HARKEN_DENORMAL_PROBE_X86 1
#include <xmmintrin.h>
#include <pmmintrin.h>
#else
#define HARKEN_DENORMAL_PROBE_X86 0
#endif

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint32_t kWarmupBlocks = 1200;
constexpr std::uint32_t kMeasureBlocks = 1600;
constexpr std::uint32_t kSourceFrames =
    (kWarmupBlocks + kMeasureBlocks + 64) * kBlockFrames;
constexpr float kDelayMs = 1.0f;
constexpr float kDelayFeedback = 0.99f;
constexpr float kPeakGainDb = 24.0f;
constexpr float kPeakFrequencyHz = 1000.0f;
constexpr float kPeakQ = 100.0f;

struct ArmResult {
    const char* name = nullptr;
    std::uint32_t mxcsr = 0;
    double totalUs = 0.0;
    double meanUs = 0.0;
    double p99Us = 0.0;
    double p999Us = 0.0;
    double peakUs = 0.0;
    std::uint32_t audioCpuPeakUs = 0;
    bool ok = false;
    const char* error = nullptr;
};

class EngineGuard {
public:
    explicit EngineGuard(bool active) : m_active(active) {}
    ~EngineGuard() {
        if (m_active) {
            Vn_Term();
        }
    }

    EngineGuard(const EngineGuard&) = delete;
    EngineGuard& operator=(const EngineGuard&) = delete;

private:
    bool m_active;
};

class ScopedPumpFloatMode {
public:
    explicit ScopedPumpFloatMode(bool enableFtzDaz) {
#if HARKEN_DENORMAL_PROBE_X86
        m_previous = _mm_getcsr();
        if (enableFtzDaz) {
            _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
            _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
        }
#else
        (void)enableFtzDaz;
#endif
    }

    ~ScopedPumpFloatMode() {
#if HARKEN_DENORMAL_PROBE_X86
        _mm_setcsr(m_previous);
#endif
    }

    std::uint32_t Current() const {
#if HARKEN_DENORMAL_PROBE_X86
        return _mm_getcsr();
#else
        return 0;
#endif
    }

    ScopedPumpFloatMode(const ScopedPumpFloatMode&) = delete;
    ScopedPumpFloatMode& operator=(const ScopedPumpFloatMode&) = delete;

private:
    std::uint32_t m_previous = 0;
};

const VnEffectDesc* AsBase(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

std::vector<float> MakeImpulseAndSilence() {
    std::vector<float> samples(kSourceFrames, 0.0f);
    samples.front() = 0.25f;
    return samples;
}

std::size_t PercentileIndex(std::size_t count, std::size_t numerator,
                            std::size_t denominator) {
    return (count * numerator + denominator - 1) / denominator - 1;
}

ArmResult RunArm(const char* name, bool enableFtzDaz) {
    ArmResult result;
    result.name = name;

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    config.maxVoices = 1;
    config.fxSlotsPerVoice = 2;
    config.delayEffectPoolCapacity = 1;
    config.toneEffectPoolCapacity = 1;
    config.blockFrames = kBlockFrames;
    config.sampleRate = kSampleRate;
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.renderWorkerCount = 0;
    config.arenaBytes = 1u << 20;

    if (Vn_Init(&config) != VN_OK) {
        result.error = "Vn_Init failed";
        return result;
    }
    EngineGuard guard(true);
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        result.error = "active engine missing";
        return result;
    }

    const std::vector<float> source = MakeImpulseAndSilence();
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(VnPCMDesc);
    pcm.sampleRate = kSampleRate;
    pcm.channels = 1;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_COPY;
    pcm.frames = kSourceFrames;
    pcm.samplesInterleaved = source.data();
    if (Vn_RegisterPCM(harkentest::ObjectId(1), &pcm) != VN_OK) {
        result.error = "Vn_RegisterPCM failed";
        return result;
    }

    VnPlayDesc play{};
    play.structSize = sizeof(VnPlayDesc);
    play.soundId = harkentest::ObjectId(1);
    play.busId = VN_BUS_MASTER;
    play.pitchRatio = 1.0f;
    play.loopCount = 1;
    VnVoiceHandle handle = 0;
    if (harkentest::PlayRaw(&play, &handle) != VN_OK || handle == 0) {
        result.error = "raw voice start failed";
        return result;
    }

    VnFeedbackDelayDesc delay{};
    delay.structSize = sizeof(VnFeedbackDelayDesc);
    delay.delayMs = kDelayMs;
    delay.feedback = kDelayFeedback;
    delay.wet = 1.0f;
    delay.dry = 0.0f;
    if (Vn_SetVoiceEffect(handle, 0, VN_EFFECT_FEEDBACK_DELAY,
                          AsBase(&delay)) != VN_OK) {
        result.error = "delay setup failed";
        return result;
    }

    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = 0.0f;
    tone.lowShelfFreqHz = 200.0f;
    tone.peakGainDb = kPeakGainDb;
    tone.peakFreqHz = kPeakFrequencyHz;
    tone.peakQ = kPeakQ;
    tone.highShelfGainDb = 0.0f;
    tone.highShelfFreqHz = 6000.0f;
    if (Vn_SetVoiceEffect(handle, 1, VN_EFFECT_THREE_BAND_TONE,
                          AsBase(&tone)) != VN_OK) {
        result.error = "tone setup failed";
        return result;
    }
    if (Vn_Update() != VN_OK) {
        result.error = "Vn_Update failed";
        return result;
    }

    std::array<double, kMeasureBlocks> timings{};
    {
        ScopedPumpFloatMode floatMode(enableFtzDaz);
        result.mxcsr = floatMode.Current();
        engine->nullBackend.Pump(kWarmupBlocks);
        for (std::uint32_t block = 0; block < kMeasureBlocks; ++block) {
            const auto started = std::chrono::steady_clock::now();
            engine->nullBackend.Pump(1);
            const auto finished = std::chrono::steady_clock::now();
            const double elapsedUs =
                std::chrono::duration<double, std::micro>(finished - started)
                    .count();
            timings[block] = elapsedUs;
            result.totalUs += elapsedUs;
        }
    }

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    if (Vn_CaptureSnapshot(&snapshot) != VN_OK) {
        result.error = "Vn_CaptureSnapshot failed";
        return result;
    }
    if (snapshot.activeVoiceCount != 1) {
        result.error = "voice ended during measurement";
        return result;
    }
    result.audioCpuPeakUs = snapshot.audioCpuPeakUs;

    std::sort(timings.begin(), timings.end());
    result.meanUs = result.totalUs / static_cast<double>(kMeasureBlocks);
    result.p99Us = timings[PercentileIndex(kMeasureBlocks, 99, 100)];
    result.p999Us = timings[PercentileIndex(kMeasureBlocks, 999, 1000)];
    result.peakUs = timings.back();
    result.ok = true;
    return result;
}

void PrintRow(const ArmResult& result) {
    const bool ftz = (result.mxcsr & 0x8000u) != 0;
    const bool daz = (result.mxcsr & 0x0040u) != 0;
    std::printf("%-10s 0x%08x %3s %3s %12.3f %10.3f %10.3f %10.3f "
                "%10.3f %13u\n",
                result.name, result.mxcsr, ftz ? "on" : "off",
                daz ? "on" : "off", result.totalUs / 1000.0, result.meanUs,
                result.p99Us, result.p999Us, result.peakUs,
                result.audioCpuPeakUs);
}

} // namespace

int main() {
#if !HARKEN_DENORMAL_PROBE_X86
    std::fprintf(stderr,
                 "FTZ/DAZ mode control is unavailable on this architecture\n");
    return 2;
#else
    const ArmResult defaultMode = RunArm("default", false);
    if (!defaultMode.ok) {
        std::fprintf(stderr, "FAIL default arm: %s\n",
                     defaultMode.error != nullptr ? defaultMode.error
                                                  : "unknown error");
        return 2;
    }
    const ArmResult ftzDaz = RunArm("FTZ+DAZ", true);
    if (!ftzDaz.ok) {
        std::fprintf(stderr, "FAIL FTZ+DAZ arm: %s\n",
                     ftzDaz.error != nullptr ? ftzDaz.error
                                             : "unknown error");
        return 2;
    }

    std::printf("Denormal probe: %u Hz, %u frames, warmup %u, measure %u\n",
                kSampleRate, kBlockFrames, kWarmupBlocks, kMeasureBlocks);
    std::printf("scene=mono impulse then silence; delay_ms=%.1f feedback=%.2f "
                "wet=1 dry=0; peak_gain_db=%.1f peak_hz=%.1f peak_q=%.1f\n",
                kDelayMs, kDelayFeedback, kPeakGainDb, kPeakFrequencyHz,
                kPeakQ);
#ifdef NDEBUG
    std::printf("build_mode=NDEBUG ");
#else
    std::printf("build_mode=debug ");
#endif
    std::printf("timed_region=Pump(1) threshold=none\n");
    std::printf("%-10s %-10s %3s %3s %12s %10s %10s %10s %10s %13s\n",
                "mode", "mxcsr", "ftz", "daz", "total_ms", "mean_us",
                "p99_us", "p999_us", "peak_us", "audio_peak_us");
    PrintRow(defaultMode);
    PrintRow(ftzDaz);
    return 0;
#endif
}
