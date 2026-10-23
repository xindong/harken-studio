// Harken host CLI: drives the milestone demo scenario against the real
// engine — load two wav files, play one 2D and one 3D voice, and walk a
// phased script so each acceptance-relevant behavior gets its own
// audible window: (A) the 3D emitter flies by so attenuation and panning
// act, (B) the master bus volume ramps down and back so bus ramps are
// heard, (C) a Variable sweeps the 2D voice's lowpass cutoff through a
// curve binding, (D) both voices fade-stop. Snapshot facts print while
// running and a final summary reports voice states and high-water marks;
// the run is judged non-silent from the master bus peak. Not part of the
// shipping library; links the engine like a game would (public C API),
// plus the internal surfaces the milestone design reserves for host and
// tests: the null-backend pump (the test-clock precedent) and the
// parameter binding assembly.
//
// Exit codes: 0 = completed and non-silent; 1 = usage or file error;
// 2 = SILENT verdict (the run never moved the master bus above the
// silence floor); 3 = engine call failure.
#include <harken/Harken.h>

#include "Att03KeepProbe.h"
#include "Att04NearFarProbe.h"
#include "DemoOptions.h"
#include "DownmixNullDrill.h"
#include "PackPlay.h"
#include "PreviewPlay.h"
#include "RemoteServe.h"
#include "SchedulingMeasure.h"
#include "TraceReplay.h"
#include "JsonWriter.h"
#include "harken/core/AllocGuard.h"
#include "harken/media/StreamFormat.h"
#include "Engine.h" // internal: --null mode drives blocks via the pump
#include "HarkenApiInternal.h"
#include "WavLoader.h" // the host's wav loading path (miniaudio decode)
#include "WavWriter.h"
#include "harken/dsp/Biquad.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/render/BuiltinEffects.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kSilenceFloor = 1e-4f;
constexpr VnSoundId kSound2D{0x76656e7573686f73, 1};
constexpr VnSoundId kSound3D{0x76656e7573686f73, 2};
constexpr VnAttenuationId kProfileId{0x76656e7573686f73, 1};
// The Variable that drives the 2D voice's lowpass cutoff in phase C.
constexpr VnVariableId kCutoffVariable{0x76656e7573686f73, 7};

// One control tick of the demo script: position update + engine pump.
constexpr double kTickSeconds = 0.010;
// Status line cadence, in ticks (every half second).
constexpr int kPrintEveryTicks = 50;
constexpr double kSceneMinSeconds = 8.0;
constexpr double kSceneMaxSeconds = 300.0;
constexpr double kSceneOneShotStartRatio = 0.45;
constexpr double kSceneMusicMinOverlapSeconds = 0.10;
constexpr double kSceneMusicMaxTailSeconds = 6.0;
constexpr double kSceneMusicEndToleranceSeconds = 1.0;

struct Options {
    enum class Mode {
        Demo,
        StreamEndurance,
        M2Scene,
        M6Scene,
        M7S7Render,
        VariableWorldRun
    } mode = Mode::Demo;
    std::vector<std::string> wavPaths;
    std::string musicPath;
    std::string ambiencePath;
    std::string oneshotPath;
    double seconds = 6.0;
    bool nullMode = false;
    bool synth = false;
    std::string packcPath;
    std::string outDir = ".temp/m7-s7-listen";
    std::uint32_t chunkFrames = 4096;
};

void PrintUsage() {
    std::fprintf(
        stderr,
        "usage: harken_host demo --wav <fileA> --wav <fileB> [--seconds N] "
        "[--null]\n"
        "       harken_host demo --synth [--seconds N] [--null]\n"
        "       harken_host stream-endurance [--seconds N] [--packc <exe>] "
        "[--chunk-frames N] [--null]\n"
        "       harken_host m2-scene [--seconds N] [--packc <exe>] "
        "[--chunk-frames N] [--music <wav>] [--ambience <wav>] "
        "[--oneshot <wav>] [--null]\n"
        "       harken_host m6-scene [--seconds N] [--null]\n"
        "       harken_host m7-s7-render [--out <dir>]\n"
        "       harken_host variable-world-run [--seconds N] [--out <dir>]\n"
        "       harken_host sched-measure --workers <0|1|2|4|8> "
        "--load <light|medium|heavy> --seconds N --csv <path> "
        "--cpu-duty <0|50|90> [--cpu-threads N]\n"
        "       harken_host remote-serve [--pipe <name>] [--seconds N] "
        "[--null]\n"
        "       harken_host trace-replay <file> [--null] "
        "[--capture <raw-f32-out>]\n"
        "       harken_host pack-play --root <platform-dir> --action-name <name> "
        "[--language NAME] [--seconds N] [--repeats N] "
        "[--start-delay-ms N] "
        "[--capture-ready-gate <path> --capture-ready-timeout-ms N] "
        "[--caller-seed N] "
        "[--variable-id ID --variable-values v1,v2] "
        "[--no-recovery] [--null]\n"
        "       harken_host preview-play [--describe] [--null] [--seconds N] "
        "[--mode random|rotation|sequence|random-queue|sequence-queue|"
        "variable-discrete|variable-continuous|variable-dual|track|piece]\n"
        "       harken_host att03-keep --init <Init.pack> --content <Content.pack> "
        "--bad <missing-or-bad.pack> --action <stableId-low>\n"
        "       harken_host att04-nearfar --root <platform-dir> --action-name <name> "
        "--distance <meters> [--language NAME] [--seconds N] --null\n"
        "\n"
        "  --wav <file>   a wav source; given exactly twice. The first\n"
        "                 plays as the 2D voice, the second as the moving\n"
        "                 3D voice. Mono and stereo files are supported\n"
        "                 (any sample rate; the engine resamples). A\n"
        "                 stereo 3D source is downmixed to mono.\n"
        "  --synth        synthesize the two sources instead, write them\n"
        "                 to .temp/ as wav files, and load them back\n"
        "                 through the same wav path.\n"
        "  --seconds N    demo length in seconds (default 6, max 3600).\n"
        "  --null         headless smoke mode: null device, blocks driven\n"
        "                 by an internal clock - no audio hardware.\n");
    std::fprintf(stderr,
                 "\nstream-endurance writes a low-level source under .temp/,\n"
                 "compiles it through packc --stream, streams it, polls\n"
                 "Vn_CaptureSnapshot, and exits on the ended notification.\n");
    std::fprintf(stderr,
                 "\nm2-scene plays a streamed music sound, an in-memory\n"
                 "looping ambience sound, and a delayed one-shot together;\n"
                 "the three wav arguments override the synthesized defaults.\n"
                 "m2-scene --seconds defaults to 40 and accepts [8, 300].\n"
                 "Override wavs play as authored with no normalization; hot\n"
                 "content can clip. --music must cover the one-shot trigger\n"
                 "(45%% of --seconds plus 0.10 s) and end by --seconds + 6 s.\n");
    std::fprintf(stderr,
                 "\nm6-scene is a meter-reactive sidechain listen: synthesized\n"
                 "dialogue alternates three seconds on / three seconds off while\n"
                 "a continuous synthesized music bed dips by -12 dB and\n"
                 "recovers. --seconds defaults to 24 and accepts [8, 300].\n");
    std::fprintf(stderr,
                 "\nvariable-world-run installs a definitions-only Variable\n"
                 "world (Step + Smooth), drives durable Global/emitter writes\n"
                 "through voice, bus, and effect Bindings, and dips a tone bus\n"
                 "through a private MeterProbe sidechain on the null backend.\n"
                 "It writes interleaved raw float32 capture under --out and\n"
                 "requires non-silence, Global fallback, effect gain, and an\n"
                 "observable sidechain dip.\n");
    std::fprintf(
        stderr,
        "\nm7-s7-render writes deterministic Phase 1 listening stimuli:\n"
        "below-Ceiling Limiter reference/active files and static/rapid\n"
        "high-Q Tone files. Runtime rendering uses the null backend; the\n"
        "user plays the WAV files through an actual output device and records\n"
        "that playback platform/backend. --out defaults to\n"
        ".temp/m7-s7-listen.\n");
    harkenhost::PrintRemoteServeUsage();
    harkenhost::PrintTraceReplayUsage();
    harkenhost::PrintDownmixNullDrillUsage();
    harkenhost::PrintPackPlayUsage();
    harkenhost::PrintPreviewPlayUsage();
    harkenhost::PrintAtt03KeepProbeUsage();
    harkenhost::PrintAtt04NearFarProbeUsage();
}

bool ModeUsesPackc(Options::Mode mode) {
    return mode == Options::Mode::StreamEndurance ||
           mode == Options::Mode::M2Scene;
}

bool ParseChunkFrames(const char* text, std::uint32_t* out) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 ||
        value > harken::kMaxStreamChunkFrames) {
        std::fprintf(stderr,
                     "error: --chunk-frames must be an integer in "
                     "[1, %u]\n",
                     harken::kMaxStreamChunkFrames);
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

bool ParseOptions(int argc, char** argv, Options* out) {
    if (argc < 2) {
        return false;
    }
    if (std::strcmp(argv[1], "demo") == 0) {
        out->mode = Options::Mode::Demo;
        out->seconds = 6.0;
    } else if (std::strcmp(argv[1], "stream-endurance") == 0) {
        out->mode = Options::Mode::StreamEndurance;
        out->seconds = 600.0;
    } else if (std::strcmp(argv[1], "m2-scene") == 0) {
        out->mode = Options::Mode::M2Scene;
        out->seconds = 40.0;
    } else if (std::strcmp(argv[1], "m6-scene") == 0) {
        out->mode = Options::Mode::M6Scene;
        out->seconds = 24.0;
    } else if (std::strcmp(argv[1], "m7-s7-render") == 0) {
        out->mode = Options::Mode::M7S7Render;
    } else if (std::strcmp(argv[1], "variable-world-run") == 0) {
        out->mode = Options::Mode::VariableWorldRun;
        out->seconds = 8.0;
        out->nullMode = true;
        out->outDir = ".temp/variable-world-run";
    } else {
        return false;
    }
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (out->mode == Options::Mode::Demo &&
            arg == "--wav" && i + 1 < argc) {
            out->wavPaths.push_back(argv[++i]);
        } else if (out->mode != Options::Mode::M7S7Render &&
                   arg == "--seconds" && i + 1 < argc) {
            if (!harkenhost::ParseSeconds(argv[++i], &out->seconds)) {
                std::fprintf(stderr,
                             "error: --seconds must be a finite number in "
                             "(0, %.0f]\n",
                             harkenhost::kMaxDemoSeconds);
                return false;
            }
        } else if (arg == "--null" &&
                   out->mode != Options::Mode::M7S7Render) {
            out->nullMode = true;
        } else if ((out->mode == Options::Mode::M7S7Render ||
                    out->mode == Options::Mode::VariableWorldRun) &&
                   arg == "--out" && i + 1 < argc) {
            out->outDir = argv[++i];
        } else if (out->mode == Options::Mode::Demo && arg == "--synth") {
            out->synth = true;
        } else if (ModeUsesPackc(out->mode) && arg == "--packc" &&
                   i + 1 < argc) {
            out->packcPath = argv[++i];
        } else if (ModeUsesPackc(out->mode) && arg == "--chunk-frames" &&
                   i + 1 < argc) {
            if (!ParseChunkFrames(argv[++i], &out->chunkFrames)) {
                return false;
            }
        } else if (out->mode == Options::Mode::M2Scene &&
                   arg == "--music" && i + 1 < argc) {
            out->musicPath = argv[++i];
        } else if (out->mode == Options::Mode::M2Scene &&
                   arg == "--ambience" && i + 1 < argc) {
            out->ambiencePath = argv[++i];
        } else if (out->mode == Options::Mode::M2Scene &&
                   arg == "--oneshot" && i + 1 < argc) {
            out->oneshotPath = argv[++i];
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n",
                         arg.c_str());
            return false;
        }
    }
    if (out->mode == Options::Mode::StreamEndurance) {
        if (out->seconds < 600.0) {
            std::fprintf(stderr,
                         "error: stream-endurance --seconds must be at "
                         "least 600\n");
            return false;
        }
        return true;
    }
    if (out->mode == Options::Mode::M7S7Render) {
        return !out->outDir.empty();
    }
    if (out->mode == Options::Mode::VariableWorldRun) {
        return out->seconds > 0.0 && out->seconds <= kSceneMaxSeconds;
    }
    if (out->mode == Options::Mode::M2Scene ||
        out->mode == Options::Mode::M6Scene) {
        if (out->seconds < kSceneMinSeconds ||
            out->seconds > kSceneMaxSeconds) {
            std::fprintf(stderr,
                         "error: scene --seconds must be in [%.0f, %.0f]\n",
                         kSceneMinSeconds, kSceneMaxSeconds);
            return false;
        }
        return true;
    }
    if (out->synth && !out->wavPaths.empty()) {
        std::fprintf(stderr, "error: --synth and --wav are exclusive\n");
        return false;
    }
    if (!out->synth && out->wavPaths.size() != 2) {
        std::fprintf(stderr,
                     "error: give --wav exactly twice, or use --synth\n");
        return false;
    }
    return true;
}

using harkenhost::LoadedWav;
using harkenhost::LoadWav;

// Synthesizes the two demo sources and writes them under .temp/ as
// ordinary wav files; the caller loads them back through LoadWav so the
// file path is exercised for real.
bool SynthesizeSources(double seconds, std::string* outPathA,
                       std::string* outPathB) {
    std::error_code ec;
    std::filesystem::create_directories(".temp", ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot create .temp/: %s\n",
                     ec.message().c_str());
        return false;
    }
    const std::uint32_t rate = 48000;
    // Pad past --seconds so the sources outlast the phase D fade-stop
    // drain (1.2 s ramp + margin); otherwise the tail of the fade is
    // cut by source exhaustion instead of reaching silence on the ramp.
    const std::size_t frames =
        static_cast<std::size_t>((seconds + 2.0) * rate);

    // A: mono broadband white noise — the stationary 2D voice. The
    // lowpass sweep needs energy across the whole spectrum to be
    // audible; a pure tone would make the sweep undecidable by ear.
    // Deterministic xorshift32 with a fixed seed keeps the source
    // identical run-to-run.
    std::vector<float> a(frames);
    std::uint32_t rng = 0x12345678u;
    for (std::size_t n = 0; n < frames; ++n) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        // Map an exactly representable 24-bit slice of the state to
        // [-1, 1) — a full 32-bit cast can round up to +1.0 — then
        // scale to 0.25 peak.
        const float uniform =
            static_cast<float>(rng >> 8) * (1.0f / 8388608.0f) - 1.0f;
        a[n] = 0.25f * uniform;
    }
    // B: stereo harmonic stack at 220 Hz (harmonics 1..12, amplitude
    // 1/k — saw-like, band-limited far below Nyquist) — the flyby
    // voice. Rich harmonics make air-absorption dulling and width
    // collapse audible while the pitch stays trackable in motion.
    // Stereo with identical channels on purpose, so the demo's 3D
    // downmix path runs in synth mode too.
    std::vector<float> b(frames * 2);
    float stackPeak = 0.0f;
    for (std::size_t n = 0; n < frames; ++n) {
        double sum = 0.0;
        for (int k = 1; k <= 12; ++k) {
            sum += std::sin(2.0 * kPi * 220.0 * k *
                            static_cast<double>(n) / rate) /
                   static_cast<double>(k);
        }
        const float sample = static_cast<float>(sum);
        if (std::fabs(sample) > stackPeak) {
            stackPeak = std::fabs(sample);
        }
        b[n * 2 + 0] = sample;
        b[n * 2 + 1] = sample;
    }
    // Normalize the summed stack to 0.35 peak so its level matches the
    // old source regardless of how the harmonics happen to align.
    const float stackGain = stackPeak > 0.0f ? 0.35f / stackPeak : 0.0f;
    for (float& sample : b) {
        sample *= stackGain;
    }

    *outPathA = ".temp/harken_host_synth_2d.wav";
    *outPathB = ".temp/harken_host_synth_3d.wav";
    if (!harkenhost::WriteWav16(*outPathA, a, 1, rate) ||
        !harkenhost::WriteWav16(*outPathB, b, 2, rate)) {
        std::fprintf(stderr, "error: cannot write synth wavs to .temp/\n");
        return false;
    }
    std::printf("synth: wrote %s (mono white noise) and %s (stereo 220 Hz "
                "harmonic stack)\n",
                outPathA->c_str(), outPathB->c_str());
    return true;
}

bool Check(VnResult result, const char* what) {
    if (result != VN_OK) {
        std::fprintf(stderr, "error: %s failed (VnResult %d)\n", what,
                     static_cast<int>(result));
        return false;
    }
    return true;
}

std::vector<float> MakeM7LimiterSource(std::size_t frames) {
    constexpr double rate = 48000.0;
    std::vector<float> source(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / rate;
        const double stack =
            std::sin(2.0 * kPi * 220.0 * t) +
            0.35 * std::sin(2.0 * kPi * 440.0 * t + 0.2) +
            0.18 * std::sin(2.0 * kPi * 880.0 * t + 0.7);
        source[n] = 0.16f * static_cast<float>(stack);
    }
    return source;
}

std::vector<float> MakeM7ToneSource(std::size_t frames) {
    constexpr double rate = 48000.0;
    std::vector<float> source(frames);
    std::uint32_t rng = 0x7f4a7c15u;
    for (std::size_t n = 0; n < frames; ++n) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const float noise =
            static_cast<float>(rng >> 8) * (1.0f / 8388608.0f) - 1.0f;
        const double t = static_cast<double>(n) / rate;
        const float harmonic = static_cast<float>(
            std::sin(2.0 * kPi * 110.0 * t) +
            0.5 * std::sin(2.0 * kPi * 330.0 * t));
        source[n] = 0.10f * noise + 0.06f * harmonic;
    }
    return source;
}

bool AppendNullBlock(harken::Engine& engine, std::vector<float>* output,
                     std::uint32_t* channels) {
    const std::uint32_t blockChannels = engine.nullBackend.Channels();
    const std::uint32_t blockFrames = engine.nullBackend.BlockFrames();
    if (blockChannels == 0 || blockFrames == 0) {
        return false;
    }
    if (*channels == 0) {
        *channels = blockChannels;
    } else if (*channels != blockChannels) {
        return false;
    }
    const float* block = engine.nullBackend.LastBlock();
    output->insert(output->end(), block,
                   block + static_cast<std::size_t>(blockFrames) *
                               blockChannels);
    return true;
}

bool RenderM7LimiterStimulus(bool bypass, std::vector<float>* output,
                             std::uint32_t* channels) {
    constexpr std::uint32_t rate = 48000;
    constexpr std::uint32_t blockFrames = 256;
    constexpr std::uint32_t blocks = 1125;
    constexpr VnSoundId sound{0x76656e7573686f73, 9701};

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.sampleRate = rate;
    config.blockFrames = blockFrames;
    config.maxVoices = 1;
    config.fxSlotsPerVoice = 1;
    config.limiterEffectPoolCapacity = 1;
    config.deviceMode = VN_DEVICE_NULL;
    if (!Check(Vn_Init(&config), "Vn_Init(m7 limiter)")) {
        return false;
    }

    const std::vector<float> source =
        MakeM7LimiterSource(static_cast<std::size_t>(blocks) * blockFrames);
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(VnPCMDesc);
    pcm.sampleRate = rate;
    pcm.channels = 1;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_COPY;
    pcm.frames = source.size();
    pcm.samplesInterleaved = source.data();
    VnPlayDesc play{};
    play.structSize = sizeof(VnPlayDesc);
    play.soundId = sound;
    play.busId = VN_BUS_MASTER;
    play.pitchRatio = 1.0f;
    play.loopCount = 1;
    VnVoiceHandle handle = 0;
    VnLimiterDesc limiter{};
    limiter.structSize = sizeof(VnLimiterDesc);
    limiter.ceilingDb = -6.0f;
    limiter.releaseMs = 100.0f;
    if (!Check(Vn_RegisterPCM(sound, &pcm), "Vn_RegisterPCM(m7 limiter)") ||
        !Check(harken::detail::PlayVoiceRaw(&play, &handle),
               "PlayVoiceRaw(m7 limiter)") ||
        !Check(Vn_SetVoiceEffectBypass(handle, 0, bypass ? 1u : 0u),
               "Vn_SetVoiceEffectBypass(m7 limiter)") ||
        !Check(Vn_SetVoiceEffect(
                   handle, 0, VN_EFFECT_LIMITER,
                   reinterpret_cast<const VnEffectDesc*>(&limiter)),
               "Vn_SetVoiceEffect(m7 limiter)") ||
        !Check(Vn_Update(), "Vn_Update(m7 limiter)")) {
        Vn_Term();
        return false;
    }

    harken::Engine* engine = harken::detail::ActiveEngine();
    bool ok = engine != nullptr;
    for (std::uint32_t block = 0; block < blocks && ok; ++block) {
        engine->nullBackend.Pump(1);
        ok = AppendNullBlock(*engine, output, channels);
    }
    Vn_Term();
    return ok;
}

bool RenderM7ToneStimulus(bool rapid, std::vector<float>* output,
                          std::uint32_t* channels) {
    constexpr std::uint32_t rate = 48000;
    constexpr std::uint32_t blockFrames = 256;
    constexpr std::uint32_t warmupBlocks = 4;
    constexpr std::uint32_t measuredBlocks = 2250;
    constexpr std::uint32_t transitionFrames = 480;
    constexpr float q = 10.0f;
    constexpr VnSoundId sound{0x76656e7573686f73, 9702};

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.sampleRate = rate;
    config.blockFrames = blockFrames;
    config.maxVoices = 1;
    config.deviceMode = VN_DEVICE_NULL;
    if (!Check(Vn_Init(&config), "Vn_Init(m7 tone)")) {
        return false;
    }

    const std::size_t sourceFrames =
        static_cast<std::size_t>(warmupBlocks + measuredBlocks + 2) *
        blockFrames;
    const std::vector<float> source = MakeM7ToneSource(sourceFrames);
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(VnPCMDesc);
    pcm.sampleRate = rate;
    pcm.channels = 1;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_COPY;
    pcm.frames = source.size();
    pcm.samplesInterleaved = source.data();
    VnPlayDesc play{};
    play.structSize = sizeof(VnPlayDesc);
    play.soundId = sound;
    play.busId = VN_BUS_MASTER;
    play.pitchRatio = 1.0f;
    play.loopCount = 1;
    VnVoiceHandle handle = 0;
    if (!Check(Vn_RegisterPCM(sound, &pcm), "Vn_RegisterPCM(m7 tone)") ||
        !Check(harken::detail::PlayVoiceRaw(&play, &handle),
               "PlayVoiceRaw(m7 tone)") ||
        !Check(Vn_Update(), "Vn_Update(m7 tone)")) {
        Vn_Term();
        return false;
    }

    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        Vn_Term();
        return false;
    }
    engine->nullBackend.Pump(1);
    const harken::BiquadCoeffs initial =
        harken::MakeLowpass(static_cast<float>(rate), 1000.0f, q);
    engine->voices.ApplySetFilter(
        handle, harken::kFilterKindLowpass, true, 1000.0f, q, initial,
        harken::EstimateBiquadTailFrames(initial), 1);
    for (std::uint32_t block = 0; block < warmupBlocks; ++block) {
        engine->nullBackend.Pump(1);
    }

    bool ok = true;
    for (std::uint32_t block = 0; block < measuredBlocks && ok; ++block) {
        if (rapid && (block % 2u) == 0) {
            const std::uint32_t position = (block / 2u) % 512u;
            const double phase =
                position < 256u
                    ? static_cast<double>(position) / 255.0
                    : static_cast<double>(511u - position) / 255.0;
            const float frequency =
                200.0f * static_cast<float>(std::pow(60.0, phase));
            const harken::BiquadCoeffs coeffs =
                harken::MakeLowpass(static_cast<float>(rate), frequency, q);
            engine->voices.ApplySetFilter(
                handle, harken::kFilterKindLowpass, true, frequency, q, coeffs,
                harken::EstimateBiquadTailFrames(coeffs), transitionFrames);
        }
        engine->nullBackend.Pump(1);
        ok = AppendNullBlock(*engine, output, channels);
    }
    Vn_Term();
    return ok;
}

int RunM7S7Render(const Options& options) {
    std::error_code ec;
    std::filesystem::create_directories(options.outDir, ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot create %s: %s\n",
                     options.outDir.c_str(), ec.message().c_str());
        return 1;
    }

    const std::filesystem::path base(options.outDir);
    const std::filesystem::path limiterReferencePath =
        base / "limiter_below_ceiling_reference.wav";
    const std::filesystem::path limiterActivePath =
        base / "limiter_below_ceiling_active.wav";
    const std::filesystem::path toneStaticPath =
        base / "tone_static_high_q_reference.wav";
    const std::filesystem::path toneRapidPath =
        base / "tone_rapid_high_q.wav";
    const auto cleanupOutputs = [&]() {
        bool clean = true;
        std::error_code removeError;
        std::filesystem::remove(limiterReferencePath, removeError);
        clean = clean && !removeError;
        removeError.clear();
        std::filesystem::remove(limiterActivePath, removeError);
        clean = clean && !removeError;
        removeError.clear();
        std::filesystem::remove(toneStaticPath, removeError);
        clean = clean && !removeError;
        removeError.clear();
        std::filesystem::remove(toneRapidPath, removeError);
        clean = clean && !removeError;
        return clean;
    };
    if (!cleanupOutputs()) {
        std::fprintf(stderr,
                     "error: cannot clear prior M7 S7 listening WAVs\n");
        return 1;
    }

    std::vector<float> limiterReference;
    std::vector<float> limiterActive;
    std::vector<float> toneStatic;
    std::vector<float> toneRapid;
    std::uint32_t limiterReferenceChannels = 0;
    std::uint32_t limiterActiveChannels = 0;
    std::uint32_t toneStaticChannels = 0;
    std::uint32_t toneRapidChannels = 0;
    if (!RenderM7LimiterStimulus(true, &limiterReference,
                                 &limiterReferenceChannels) ||
        !RenderM7LimiterStimulus(false, &limiterActive,
                                 &limiterActiveChannels) ||
        !RenderM7ToneStimulus(false, &toneStatic, &toneStaticChannels) ||
        !RenderM7ToneStimulus(true, &toneRapid, &toneRapidChannels)) {
        cleanupOutputs();
        return 3;
    }
    const bool limiterIdentical =
        limiterReferenceChannels == limiterActiveChannels &&
        limiterReference.size() == limiterActive.size() &&
        std::memcmp(limiterReference.data(), limiterActive.data(),
                    limiterReference.size() * sizeof(float)) == 0;
    if (!limiterIdentical) {
        std::fprintf(stderr,
                     "error: below-Ceiling Limiter stimuli are not "
                     "bit-identical\n");
        cleanupOutputs();
        return 3;
    }
    if (toneStatic == toneRapid) {
        std::fprintf(stderr,
                     "error: static and rapid Tone stimuli are identical\n");
        cleanupOutputs();
        return 3;
    }

    if (!harkenhost::WriteWav16(limiterReferencePath.string(),
                               limiterReference, limiterReferenceChannels,
                               48000) ||
        !harkenhost::WriteWav16(limiterActivePath.string(), limiterActive,
                               limiterActiveChannels, 48000) ||
        !harkenhost::WriteWav16(toneStaticPath.string(), toneStatic,
                               toneStaticChannels, 48000) ||
        !harkenhost::WriteWav16(toneRapidPath.string(), toneRapid,
                               toneRapidChannels, 48000)) {
        std::fprintf(stderr, "error: cannot write M7 S7 listening WAVs\n");
        cleanupOutputs();
        return 1;
    }

    std::printf(
        "m7-s7-render: wrote four 48 kHz PCM16 stimuli under %s\n"
        "  Limiter reference/active: 6.0 s, below -6 dB Ceiling, "
        "bit-identical=%s\n"
        "  Tone static/rapid: 12.0 s, Q=10, rapid LPF 200..12000 Hz, "
        "10 ms glide\n"
        "  Runtime backend: VN_DEVICE_NULL; audible verdict remains "
        "PENDING-USER-LISTEN\n",
        options.outDir.c_str(), limiterIdentical ? "true" : "false");
    return 0;
}

// The 3D emitter's scripted path: a left-right flyby at z = -4 whose x
// swings +/-25 with a 5-second period. Distance to the origin listener
// sweeps ~4..25 — through the profile's proximity radius at the center
// and well toward its far zone at the extremes, so rolloff, air
// absorption, width collapse, near-field boost, and panning all move.
VnVec3 ScriptedPosition(double t) {
    VnVec3 position;
    position.x = static_cast<float>(25.0 * std::sin(2.0 * kPi * t / 5.0));
    position.y = 0.0f;
    position.z = -4.0f;
    return position;
}

VnAttenuationDesc DemoProfile() {
    VnAttenuationDesc desc{};
    desc.structSize = sizeof(VnAttenuationDesc);
    desc.rolloff = VN_ROLLOFF_INVERSE_SQUARE;
    desc.minDistance = 1.0f;
    desc.maxDistance = 40.0f;
    desc.airAbsorptionNearHz = 18000.0f;
    desc.airAbsorptionFarHz = 900.0f;
    desc.proximityRadius = 6.0f;
    desc.proximityMaxBoostDb = 6.0f;
    // Cone off: the demo emitter is omnidirectional.
    return desc;
}

const char* VoiceStateName(std::uint32_t state) {
    switch (state) {
    case VN_VOICE_STATE_INACTIVE:
        return "VN_VOICE_STATE_INACTIVE";
    case VN_VOICE_STATE_ACTIVE:
        return "VN_VOICE_STATE_ACTIVE";
    case VN_VOICE_STATE_DRAINING:
        return "VN_VOICE_STATE_DRAINING";
    default:
        return "(unknown)";
    }
}

// Accumulates which lifecycle states a voice slot was ever observed in,
// so the end-of-run summary can show the full arc (active -> inactive)
// rather than only the final state.
struct VoiceStatesSeen {
    bool seen[3] = {false, false, false};

    void Observe(std::uint32_t state) {
        if (state < 3) {
            seen[state] = true;
        }
    }

    void Print(const char* label) const {
        std::printf("  %s states seen:", label);
        bool any = false;
        for (std::uint32_t s = 0; s < 3; ++s) {
            if (seen[s]) {
                std::printf("%s %s", any ? "," : "", VoiceStateName(s));
                any = true;
            }
        }
        std::printf("%s\n", any ? "" : " (none captured)");
    }
};

// The demo script's phase layout: the scripted portion of the run is
// split into three equal windows — A flyby, B master bus ramp, C
// lowpass sweep — followed by D, the fade-stop drain, which sits
// outside --seconds like the drain always has. Boundaries are in
// control ticks.
struct PhasePlan {
    int startB = 0;       // phase A is [0, startB)
    int startC = 0;       // phase B is [startB, startC)
    int totalTicks = 0;   // phase C is [startC, totalTicks)
    int rampDownTick = 0; // inside B: when the bus starts ramping down
    int rampBackTick = 0; // inside B: when the bus ramps back up
    std::uint32_t rampMs = 0; // one direction of the bus ramp

    static PhasePlan Make(int totalTicks) {
        PhasePlan plan;
        plan.totalTicks = totalTicks;
        plan.startB = totalTicks / 3;
        plan.startC = 2 * totalTicks / 3;
        // The bus dip waits ~1 s into the phase so the 2D voice's own
        // return ramp settles first and the dip is heard on a stable
        // mix; the wait shrinks when the remaining window could not
        // hold two floor-length ramps before phase C.
        const int delayTicks = static_cast<int>(1.0 / kTickSeconds);
        const int floorRampTicks = static_cast<int>(0.8 / kTickSeconds);
        plan.rampDownTick = plan.startB + delayTicks;
        const int latestDown = plan.startC - 2 * floorRampTicks;
        if (plan.rampDownTick > latestDown) {
            plan.rampDownTick = latestDown;
        }
        if (plan.rampDownTick < plan.startB) {
            plan.rampDownTick = plan.startB;
        }
        // Ramp down for 40% of the remaining window, hold, ramp back
        // over the last 40%. Tick arithmetic first: rampMs is derived
        // from a whole tick count (floored at the 800 ms = 80-tick
        // ramp), so the ramp-back start is tick-exact and the up-ramp
        // finishes exactly at phase C. When --seconds is too short to
        // hold two floor-length ramps, rampBackTick collapses onto
        // rampDownTick and the ramp-up queued on the same control tick
        // supersedes the dip — the script stays valid but the bus dip
        // becomes inaudible; the acceptance run uses --seconds 24,
        // where the schedule is clean.
        const int windowTicks = plan.startC - plan.rampDownTick;
        int rampTicks = (windowTicks * 2) / 5;
        if (rampTicks < floorRampTicks) {
            rampTicks = floorRampTicks;
        }
        plan.rampMs =
            static_cast<std::uint32_t>(rampTicks * kTickSeconds * 1000.0);
        plan.rampBackTick = plan.startC - rampTicks;
        if (plan.rampBackTick < plan.rampDownTick) {
            plan.rampBackTick = plan.rampDownTick;
        }
        return plan;
    }
};

// Advances real or simulated time by one control tick. In device mode
// the audio thread renders on its own and the demo simply sleeps; in
// null mode the demo IS the clock and pumps the equivalent block count,
// carrying the fractional remainder.
struct DemoClock {
    bool nullMode = false;
    harken::Engine* engine = nullptr;
    double pendingFrames = 0.0;

    void Advance() {
        if (!nullMode) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return;
        }
        pendingFrames +=
            kTickSeconds * engine->config.mixRate.SampleRate();
        const double blockFrames =
            static_cast<double>(engine->config.blockFrames);
        while (pendingFrames >= blockFrames) {
            engine->nullBackend.Pump(1);
            // The null clock consumes blocks as fast as the host can run;
            // sequence a streaming service pass after each block so stream
            // rings are fed deterministically instead of depending on the
            // streaming thread's real-time wake cadence.
            engine->streams.SyncServe();
            pendingFrames -= blockFrames;
        }
    }
};

struct HostNotificationLog {
    bool ended = false;
    std::uint64_t starved = 0;
};

void RecordHostNotification(const VnNotification* notification, void* userData) {
    HostNotificationLog* log = static_cast<HostNotificationLog*>(userData);
    if (notification->type == VN_NOTIFY_VOICE_ENDED) {
        log->ended = true;
    } else if (notification->type == VN_NOTIFY_VOICE_STARVED) {
        ++log->starved;
    }
}

std::uint64_t RoundUpPow2Host(std::uint64_t v) {
    std::uint64_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

std::string JsonEscape(const std::filesystem::path& path) {
    std::string s = std::filesystem::absolute(path).generic_string();
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

bool WriteTextFile(const std::filesystem::path& path,
                   const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

std::string QuoteArg(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

std::filesystem::path ResolvePackcPath(const Options& options,
                                       char** argv) {
    if (!options.packcPath.empty()) {
        return std::filesystem::path(options.packcPath);
    }
    std::filesystem::path exe = std::filesystem::absolute(argv[0]);
#if defined(_WIN32)
    return exe.parent_path() / "packc.exe";
#else
    return exe.parent_path() / "packc";
#endif
}

bool RunPackcStream(const std::filesystem::path& packc,
                       const std::filesystem::path& manifest) {
    const std::string command = QuoteArg(packc) + " --stream " +
                                QuoteArg(manifest);
#if defined(_WIN32)
    // cmd.exe strips the first and last quote character from the string it
    // receives unless the whole command line is wrapped in one extra pair,
    // so the quoted executable path would lose its opening quote.
    return std::system(("\"" + command + "\"").c_str()) == 0;
#else
    return std::system(command.c_str()) == 0;
#endif
}

std::string StreamManifest(VnObjectId sound, const std::filesystem::path& source,
    const std::filesystem::path& output, std::uint32_t chunkFrames) {
    harkenhost::JsonWriter json;
    json.BeginObject();
    json.Key("manifestVersion");
    json.ValueU64(3);
    json.Key("output");
    json.ValueString(std::filesystem::absolute(output).generic_string());
    json.Key("chunkFrames");
    json.ValueU64(chunkFrames);
    json.Key("entries");
    json.BeginArray();
    json.BeginObject();
    json.Key("path");
    json.ValueString(std::filesystem::absolute(source).generic_string());
    json.Key("id");
    json.ValueString(harken::ObjectIdString(sound));
    json.EndObject();
    json.EndArray();
    json.EndObject();
    return json.Finish();
}

bool WriteEnduranceWav16(const std::filesystem::path& path,
                         double seconds) {
    const std::uint32_t rate = 48000;
    const std::uint32_t channels = 1;
    const std::uint64_t frames =
        static_cast<std::uint64_t>(seconds * static_cast<double>(rate));
    const std::uint64_t dataBytes64 = frames * channels * sizeof(std::int16_t);
    if (dataBytes64 > 0xffffffffull) {
        return false;
    }
    std::FILE* file = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&file, path.string().c_str(), "wb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.string().c_str(), "wb");
#endif
    if (file == nullptr) {
        return false;
    }

    const std::uint32_t dataBytes = static_cast<std::uint32_t>(dataBytes64);
    const std::uint32_t byteRate = rate * channels * sizeof(std::int16_t);
    const std::uint16_t blockAlign =
        static_cast<std::uint16_t>(channels * sizeof(std::int16_t));
    const std::uint32_t riffSize = 36 + dataBytes;
    const std::uint32_t fmtSize = 16;
    const std::uint16_t formatPcm = 1;
    const std::uint16_t bitsPerSample = 16;
    const std::uint16_t channels16 = 1;
    bool ok = true;
    auto write = [&](const void* data, std::size_t bytes) {
        ok = ok && std::fwrite(data, 1, bytes, file) == bytes;
    };
    write("RIFF", 4);
    write(&riffSize, 4);
    write("WAVE", 4);
    write("fmt ", 4);
    write(&fmtSize, 4);
    write(&formatPcm, 2);
    write(&channels16, 2);
    write(&rate, 4);
    write(&byteRate, 4);
    write(&blockAlign, 2);
    write(&bitsPerSample, 2);
    write("data", 4);
    write(&dataBytes, 4);

    std::vector<std::int16_t> chunk(4096);
    std::uint64_t written = 0;
    while (written < frames) {
        const std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(), frames - written));
        for (std::size_t i = 0; i < n; ++i) {
            const double t =
                static_cast<double>(written + i) / static_cast<double>(rate);
            const double sample =
                0.08 * std::sin(2.0 * kPi * 220.0 * t) +
                0.03 * std::sin(2.0 * kPi * 331.0 * t);
            chunk[i] = static_cast<std::int16_t>(sample * 32767.0);
        }
        write(chunk.data(), n * sizeof(std::int16_t));
        written += n;
    }

    ok = std::fclose(file) == 0 && ok;
    return ok;
}

struct ScenePcmSource {
    std::vector<float> samples;
    std::uint32_t channels = 1;
    std::uint32_t sampleRate = 48000;
    std::uint64_t frames = 0;
};

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

bool LoadScenePcmSource(const std::string& path, const char* label,
                        ScenePcmSource* out) {
    LoadedWav wav;
    if (!LoadWav(path, &wav)) {
        return false;
    }
    out->samples = std::move(wav.samples);
    out->channels = wav.channels;
    out->sampleRate = wav.sampleRate;
    out->frames = wav.frames;
    std::printf("loaded %s: %s (%u ch, %u Hz, %llu frames)\n", label,
                path.c_str(), out->channels, out->sampleRate,
                static_cast<unsigned long long>(out->frames));
    return true;
}

VnPCMDesc ScenePcmDesc(const ScenePcmSource& source) {
    VnPCMDesc desc{};
    desc.structSize = sizeof(VnPCMDesc);
    desc.sampleRate = source.sampleRate;
    desc.channels = source.channels;
    desc.layout = VN_PCM_LAYOUT_INTERLEAVED;
    desc.policy = VN_PCM_POLICY_COPY;
    desc.frames = source.frames;
    desc.samplesInterleaved = source.samples.data();
    return desc;
}

const VnEffectDesc* EffectBase(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

std::vector<float> MakeSceneMusic(double seconds, std::uint32_t rate) {
    const std::uint64_t frames =
        static_cast<std::uint64_t>(seconds * static_cast<double>(rate));
    std::vector<float> out(static_cast<std::size_t>(frames) * 2);
    const double chords[4][4] = {
        {220.0, 261.625565, 329.627557, 391.995436},
        {174.614116, 220.0, 261.625565, 329.627557},
        {195.997718, 246.941651, 293.664768, 391.995436},
        {164.813778, 220.0, 246.941651, 329.627557},
    };
    constexpr double kNoteSeconds = 0.50;
    constexpr double kChordSeconds = 4.0;
    for (std::uint64_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / rate;
        const int chord =
            static_cast<int>(t / kChordSeconds) %
            static_cast<int>(sizeof(chords) / sizeof(chords[0]));
        const int note =
            static_cast<int>(t / kNoteSeconds) %
            static_cast<int>(sizeof(chords[0]) / sizeof(chords[0][0]));
        const double pos = std::fmod(t, kNoteSeconds) / kNoteSeconds;
        const double env = std::sin(kPi * pos);
        const double freq = chords[chord][note];
        double voice = 0.0;
        voice += std::sin(2.0 * kPi * freq * t);
        voice += 0.36 * std::sin(2.0 * kPi * freq * 2.0 * t);
        voice += 0.18 * std::sin(2.0 * kPi * freq * 3.0 * t);
        const double pan = -0.45 + 0.30 * static_cast<double>(note);
        const double leftGain = 0.76 - 0.22 * pan;
        const double rightGain = 0.76 + 0.22 * pan;
        out[static_cast<std::size_t>(n) * 2 + 0] =
            static_cast<float>(env * voice * leftGain);
        out[static_cast<std::size_t>(n) * 2 + 1] =
            static_cast<float>(env * voice * rightGain);
    }
    NormalizePeak(&out, 0.42f);
    return out;
}

ScenePcmSource MakeSceneAmbience() {
    constexpr std::uint32_t rate = 48000;
    constexpr double loopSeconds = 6.0;
    const std::uint64_t frames =
        static_cast<std::uint64_t>(loopSeconds * rate);
    ScenePcmSource source;
    source.channels = 2;
    source.sampleRate = rate;
    source.frames = frames;
    source.samples.resize(static_cast<std::size_t>(frames) * 2);
    for (std::uint64_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / rate;
        const double loopPhase = t / loopSeconds;
        const double motion =
            0.72 + 0.14 * std::sin(2.0 * kPi * loopPhase) +
            0.07 * std::sin(4.0 * kPi * loopPhase + 0.6);
        const double left =
            0.50 * std::sin(2.0 * kPi * 55.0 * t + 0.1) +
            0.32 * std::sin(2.0 * kPi * 82.5 * t + 1.2) +
            0.24 * std::sin(2.0 * kPi * 110.0 * t + 2.1);
        const double right =
            0.46 * std::sin(2.0 * kPi * 55.0 * t + 0.9) +
            0.35 * std::sin(2.0 * kPi * 82.5 * t + 0.4) +
            0.22 * std::sin(2.0 * kPi * 110.0 * t + 2.8);
        source.samples[static_cast<std::size_t>(n) * 2 + 0] =
            static_cast<float>(motion * left);
        source.samples[static_cast<std::size_t>(n) * 2 + 1] =
            static_cast<float>(motion * right);
    }
    NormalizePeak(&source.samples, 0.30f);
    return source;
}

ScenePcmSource MakeSceneOneShot() {
    constexpr std::uint32_t rate = 48000;
    constexpr double seconds = 0.55;
    const std::uint64_t frames =
        static_cast<std::uint64_t>(seconds * rate);
    ScenePcmSource source;
    source.channels = 1;
    source.sampleRate = rate;
    source.frames = frames;
    source.samples.resize(static_cast<std::size_t>(frames));
    for (std::uint64_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / rate;
        const double attack = std::min(1.0, t / 0.004);
        const double env = attack * std::exp(-8.5 * t);
        const double sample =
            std::sin(2.0 * kPi * 880.0 * t) +
            0.42 * std::sin(2.0 * kPi * 1760.0 * t + 0.25);
        source.samples[static_cast<std::size_t>(n)] =
            static_cast<float>(env * sample);
    }
    NormalizePeak(&source.samples, 0.85f);
    return source;
}

int RunStreamEndurance(const Options& options, char** argv) {
    std::error_code ec;
    std::filesystem::create_directories(".temp", ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot create .temp/: %s\n",
                     ec.message().c_str());
        return 1;
    }

    constexpr VnSoundId kStreamId{0x76656e7573686f73, 8001};
    const std::filesystem::path wavPath =
        ".temp/harken_host_stream_endurance.wav";
    const std::filesystem::path manifestPath =
        ".temp/harken_host_stream_endurance_manifest.json";
    const std::filesystem::path streamPath =
        ".temp/harken_host_stream_endurance.stream";
    if (!WriteEnduranceWav16(wavPath, options.seconds)) {
        std::fprintf(stderr, "error: cannot write endurance wav\n");
        return 1;
    }
    const std::string manifest = StreamManifest(kStreamId, wavPath, streamPath, options.chunkFrames);
    if (!WriteTextFile(manifestPath, manifest)) {
        std::fprintf(stderr, "error: cannot write endurance manifest\n");
        return 1;
    }
    const std::filesystem::path packc = ResolvePackcPath(options, argv);
    if (!RunPackcStream(packc, manifestPath)) {
        std::fprintf(stderr, "error: packc --stream failed\n");
        return 1;
    }

    HostNotificationLog notifications;
    constexpr std::uint32_t kStreamRingFrames = 16384;
    constexpr std::uint32_t kMaxStreams = 1;
    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode =
        options.nullMode ? VN_DEVICE_NULL : VN_DEVICE_REAL;
    config.streamRingFrames = kStreamRingFrames;
    config.maxStreams = kMaxStreams;
    config.onNotification = &RecordHostNotification;
    config.notificationUserData = &notifications;
    if (!Check(Vn_Init(&config), "Vn_Init")) {
        return 3;
    }
#if defined(_DEBUG) && defined(_MSC_VER)
    if (!harken::memguard::IsArmed()) {
        std::fprintf(stderr, "error: memguard is not armed\n");
        Vn_Term();
        return 3;
    }
#endif
    if (!Check(Vn_RegisterStreamFile(kStreamId, streamPath.string().c_str()),
               "Vn_RegisterStreamFile")) {
        Vn_Term();
        return 3;
    }
    VnPlayDesc play{};
    play.structSize = sizeof(VnPlayDesc);
    play.soundId = kStreamId;
    play.busId = VN_BUS_MASTER;
    play.loopCount = 1;
    VnVoiceHandle voice = 0;
    if (!Check(harken::detail::PlayVoiceRaw(&play, &voice),
               "PlayVoiceRaw(stream)")) {
        Vn_Term();
        return 3;
    }
    const auto playStarted = std::chrono::steady_clock::now();

    DemoClock clock;
    clock.nullMode = options.nullMode;
    clock.engine = harken::detail::ActiveEngine();
    const int maxTicks =
        static_cast<int>((options.seconds + 10.0) / kTickSeconds);
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    float maxPeak = 0.0f;
    int tick = 0;
    for (; tick < maxTicks && !notifications.ended; ++tick) {
        if (!Check(Vn_Update(), "Vn_Update")) {
            Vn_Term();
            return 3;
        }
        if (Vn_CaptureSnapshot(&snapshot) == VN_OK) {
            if (snapshot.busPeaks[0] > maxPeak) {
                maxPeak = snapshot.busPeaks[0];
            }
            if (tick % kPrintEveryTicks == 0) {
                std::printf("[stream %.2fs] voices=%u peak=%.5f "
                            "streamHW=%llu ringHW=%llu starved=%llu "
                            "arenaHW=%llu\n",
                            tick * kTickSeconds,
                            snapshot.activeVoiceCount,
                            snapshot.busPeaks[0],
                            static_cast<unsigned long long>(
                                snapshot.streamHighWater),
                            static_cast<unsigned long long>(
                                snapshot.streamRingHighWaterFrames),
                            static_cast<unsigned long long>(
                                snapshot.voiceStarvedCount),
                            static_cast<unsigned long long>(
                                snapshot.arenaHighWaterBytes));
            }
        }
        clock.Advance();
    }
    (void)Vn_Update();
    (void)Vn_CaptureSnapshot(&snapshot);
    const std::uint64_t guardHits = harken::memguard::HitCount();
    Vn_Term();

    const std::uint64_t roundedRingFrames =
        RoundUpPow2Host(kStreamRingFrames);
    // Null mode is driven by DemoClock, so tick time is exact audio time.
    // Real-device mode advances on the device/wall clock; sleep_for(10 ms)
    // can overshoot, so tick counting would under-report healthy runs.
    const double elapsedSeconds =
        options.nullMode
            ? tick * kTickSeconds
            : std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - playStarted)
                  .count();
    // Notification delivery is quantized to the host's 10 ms control tick in
    // null mode; the same slack is harmless in real-device mode.
    const bool ranLongEnough =
        elapsedSeconds + kTickSeconds >= options.seconds;
    std::printf("stream-endurance summary: ended=%s elapsed=%.2f "
                "ticks=%d maxPeak=%.5f "
                "streamHighWater=%llu ringHighWater=%llu "
                "voiceStarved=%llu eventStarved=%llu "
                "arenaHighWaterBytes=%llu memguardHits=%llu\n",
                notifications.ended ? "yes" : "no", elapsedSeconds, tick, maxPeak,
                static_cast<unsigned long long>(snapshot.streamHighWater),
                static_cast<unsigned long long>(
                    snapshot.streamRingHighWaterFrames),
                static_cast<unsigned long long>(snapshot.voiceStarvedCount),
                static_cast<unsigned long long>(notifications.starved),
                static_cast<unsigned long long>(
                    snapshot.arenaHighWaterBytes),
                static_cast<unsigned long long>(guardHits));
    if (!notifications.ended || !ranLongEnough || maxPeak < kSilenceFloor ||
        snapshot.streamHighWater > kMaxStreams ||
        snapshot.streamRingHighWaterFrames > roundedRingFrames ||
        snapshot.voiceStarvedCount != 0 || notifications.starved != 0 ||
        guardHits != 0) {
        return 3;
    }
    return 0;
}

struct SceneNotificationLog {
    VnVoiceHandle music = 0;
    VnVoiceHandle ambience = 0;
    VnVoiceHandle oneShot = 0;
    bool musicEnded = false;
    bool ambienceStoppedByFade = false;
    bool oneShotEnded = false;
    int currentTick = 0;
    int musicEndTick = -1;
    double currentElapsedSeconds = 0.0;
    double musicEndElapsedSeconds = -1.0;
    std::uint64_t starved = 0;
};

void RecordSceneNotification(const VnNotification* notification, void* userData) {
    SceneNotificationLog* log = static_cast<SceneNotificationLog*>(userData);
    if (notification->type == VN_NOTIFY_VOICE_STARVED) {
        ++log->starved;
        return;
    }
    if (notification->voiceHandle == log->music &&
        notification->type == VN_NOTIFY_VOICE_ENDED) {
        log->musicEnded = true;
        if (log->musicEndTick < 0) {
            log->musicEndTick = log->currentTick;
            log->musicEndElapsedSeconds = log->currentElapsedSeconds;
        }
    } else if (notification->voiceHandle == log->ambience &&
               notification->type == VN_NOTIFY_VOICE_STOPPED_BY_FADE) {
        log->ambienceStoppedByFade = true;
    } else if (notification->voiceHandle == log->oneShot &&
               notification->type == VN_NOTIFY_VOICE_ENDED) {
        log->oneShotEnded = true;
    }
}

bool SceneVoiceLive(const VnSnapshot& snapshot, VnVoiceHandle handle) {
    const std::uint32_t slot =
        static_cast<std::uint32_t>(handle & 0xffffffffu);
    if (slot >= VN_SNAPSHOT_MAX_VOICES) {
        return false;
    }
    const std::uint32_t state = snapshot.voices[slot].state;
    return state == VN_VOICE_STATE_ACTIVE ||
           state == VN_VOICE_STATE_DRAINING;
}

float SceneVoiceTapPostGain(const VnSnapshot& snapshot,
                            VnVoiceHandle handle) {
    const std::uint32_t slot =
        static_cast<std::uint32_t>(handle & 0xffffffffu);
    if (slot >= VN_SNAPSHOT_MAX_VOICES) {
        return 0.0f;
    }
    return snapshot.voices[slot].tapPostGain;
}

// A short percussive footstep with a surface-distinct timbre so the
// weighted-random variation and the grass/stone surface switch are audible.
// Grass is a soft noisy thud (low body, more noise, slower decay); stone is a
// sharp click (high tone, less noise, fast decay). Each variant shifts pitch
// and decay so repeated steps on one surface still vary.
ScenePcmSource MakeFootstep(bool stone, int variant) {
    constexpr std::uint32_t rate = 48000;
    const double seconds = stone ? 0.16 : 0.22;
    const std::uint64_t frames =
        static_cast<std::uint64_t>(seconds * rate);
    ScenePcmSource source;
    source.channels = 1;
    source.sampleRate = rate;
    source.frames = frames;
    source.samples.resize(static_cast<std::size_t>(frames));
    std::uint32_t rng =
        0x1000u + (stone ? 0x200u : 0u) + static_cast<std::uint32_t>(variant) * 0x37u;
    const double baseHz =
        stone ? (2400.0 + variant * 380.0) : (135.0 + variant * 45.0);
    const double decay = stone ? 52.0 : 20.0;
    const double noiseMix = stone ? 0.45 : 0.65;
    for (std::uint64_t n = 0; n < frames; ++n) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const double noise =
            static_cast<double>(rng >> 8) * (1.0 / 8388608.0) - 1.0;
        const double t = static_cast<double>(n) / rate;
        const double env = std::exp(-decay * t);
        const double tone = std::sin(2.0 * kPi * baseHz * t);
        const double body = (1.0 - noiseMix) * tone + noiseMix * noise;
        source.samples[static_cast<std::size_t>(n)] =
            static_cast<float>(env * body);
    }
    NormalizePeak(&source.samples, stone ? 0.80f : 0.70f);
    return source;
}

// A short melodic music phrase for the round-robin music container; the three
// phrases use distinct note sets so the round-robin cycle is audible.
ScenePcmSource MakeMusicPhrase(int index) {
    constexpr std::uint32_t rate = 48000;
    constexpr double seconds = 2.0;
    const std::uint64_t frames =
        static_cast<std::uint64_t>(seconds * rate);
    ScenePcmSource source;
    source.channels = 2;
    source.sampleRate = rate;
    source.frames = frames;
    source.samples.resize(static_cast<std::size_t>(frames) * 2);
    static const double scales[3][4] = {
        {220.0, 277.18, 329.63, 440.0},
        {246.94, 311.13, 369.99, 493.88},
        {196.0, 246.94, 293.66, 392.0},
    };
    const double* notes = scales[index % 3];
    constexpr double noteSeconds = 0.5;
    for (std::uint64_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / rate;
        const int note = static_cast<int>(t / noteSeconds) % 4;
        const double pos = std::fmod(t, noteSeconds) / noteSeconds;
        const double env = std::sin(kPi * pos);
        const double f = notes[note];
        const double v =
            std::sin(2.0 * kPi * f * t) + 0.30 * std::sin(2.0 * kPi * 2.0 * f * t);
        source.samples[static_cast<std::size_t>(n) * 2 + 0] =
            static_cast<float>(env * v * 0.7);
        source.samples[static_cast<std::size_t>(n) * 2 + 1] =
            static_cast<float>(env * v * 0.7);
    }
    NormalizePeak(&source.samples, 0.40f);
    return source;
}

ScenePcmSource MakeM6MusicBed() {
    constexpr std::uint32_t rate = 48000;
    constexpr double seconds = 6.0;
    const std::uint64_t frames =
        static_cast<std::uint64_t>(seconds * rate);
    ScenePcmSource source;
    source.channels = 1;
    source.sampleRate = rate;
    source.frames = frames;
    source.samples.resize(static_cast<std::size_t>(frames));
    for (std::uint64_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / rate;
        const double motion = 0.82 + 0.18 * std::sin(2.0 * kPi * 0.5 * t);
        const double sample =
            0.68 * std::sin(2.0 * kPi * 220.0 * t) +
            0.32 * std::sin(2.0 * kPi * 330.0 * t + 0.35);
        source.samples[static_cast<std::size_t>(n)] =
            static_cast<float>(motion * sample);
    }
    NormalizePeak(&source.samples, std::pow(10.0f, -18.0f / 20.0f));
    return source;
}

ScenePcmSource MakeM6DialogueBurst() {
    constexpr std::uint32_t rate = 48000;
    constexpr double seconds = 3.0;
    constexpr double edgeSeconds = 0.012;
    const std::uint64_t frames =
        static_cast<std::uint64_t>(seconds * rate);
    ScenePcmSource source;
    source.channels = 1;
    source.sampleRate = rate;
    source.frames = frames;
    source.samples.resize(static_cast<std::size_t>(frames));
    for (std::uint64_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / rate;
        const double remaining = seconds - t;
        const double edge =
            std::min(1.0, std::min(t / edgeSeconds, remaining / edgeSeconds));
        const double tremolo =
            0.62 + 0.38 * std::sin(2.0 * kPi * 5.0 * t);
        const double carrier =
            std::sin(2.0 * kPi * 600.0 * t) +
            0.16 * std::sin(2.0 * kPi * 1200.0 * t + 0.2);
        source.samples[static_cast<std::size_t>(n)] =
            static_cast<float>(edge * tremolo * carrier);
    }
    NormalizePeak(&source.samples, std::pow(10.0f, -12.0f / 20.0f));
    return source;
}

// Runnable scenario: install a definitions-only Variable world,
// drive Step and Smooth writes at Global and emitter scope, route them
// through voice and bus Bindings, and dip a music bus through a private
// MeterProbe sidechain. Null backend, deterministic verdicts: the run must
// be non-silent and the sidechain dip must be observable in the captured
// bus levels.
int RunVariableWorldRun(const Options& options) {
    constexpr VnSoundId kToneSound{0x76656e7573686f73, 9701};
    constexpr VnSoundId kDialogueSound{0x76656e7573686f73, 9702};
    constexpr VnBusId kToneBus{0x76656e7573686f73, 71};
    constexpr VnBusId kDialogueBus{0x76656e7573686f73, 72};
    constexpr VnVariableId kStepVariable{0x76656e7573686f73, 7701};
    constexpr VnVariableId kSmoothVariable{0x76656e7573686f73, 7702};
    constexpr VnBindingId kVoiceGainBinding{0x76656e7573686f73, 7703};
    constexpr VnBindingId kBusGainBinding{0x76656e7573686f73, 7704};
    constexpr VnObjectId kSidechainProbe{0x76656e7573686f73, 7705};
    constexpr VnObjectId kSidechainBinding{0x76656e7573686f73, 7706};
    constexpr VnBindingId kEffectGainBinding{0x76656e7573686f73, 7707};

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.sampleRate = 48000;
    config.blockFrames = 256;
    config.deviceMode = VN_DEVICE_NULL;
    config.maxVoices = 8;
    config.maxBuses = 4;
    config.maxBindings = 8;
    config.maxBindingAttachments = 8;
    config.maxEmitters = 4;
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 1;
    if (!Check(Vn_Init(&config), "Vn_Init")) {
        return 3;
    }
    harken::Engine* engine = harken::detail::ActiveEngine();

    harken::VariableDefinitionV1 definitions[2] = {};
    definitions[0].variableId = kStepVariable;
    definitions[0].kind = harken::VariableKindV1::Step;
    definitions[0].rangeMin = 0.0f;
    definitions[0].rangeMax = 4.0f;
    definitions[0].defaultValue = 0.0f;
    definitions[1].variableId = kSmoothVariable;
    definitions[1].kind = harken::VariableKindV1::Smooth;
    definitions[1].rangeMin = -24.0f;
    definitions[1].rangeMax = 0.0f;
    definitions[1].defaultValue = 0.0f;
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = definitions;
    world.variableCount = 2;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV5(world);
    const harken::WorldInstallResult installed =
        harken::InstallContainerWorldV1(*engine, world);
    if (installed.status != harken::WorldInstallStatus::Ok) {
        std::fprintf(stderr,
                     "error: definitions-only world install failed (%u)\n",
                     static_cast<unsigned>(installed.status));
        Vn_Term();
        return 3;
    }
    std::printf(
        "variable-world-run: world generation %llu (prior %llu), "
        "%u definitions installed\n",
        static_cast<unsigned long long>(installed.resultGeneration),
        static_cast<unsigned long long>(installed.priorGeneration),
        world.variableCount);

    ScenePcmSource tone = MakeM6MusicBed();
    ScenePcmSource dialogue = MakeM6DialogueBurst();
    const VnPCMDesc tonePcm = ScenePcmDesc(tone);
    const VnPCMDesc dialoguePcm = ScenePcmDesc(dialogue);
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.gainDb = 0.0f;
    if (!Check(Vn_CreateBus(kToneBus, VN_BUS_MASTER),
               "Vn_CreateBus(tone)") ||
        !Check(Vn_CreateBus(kDialogueBus, VN_BUS_MASTER),
               "Vn_CreateBus(dialogue)") ||
        !Check(Vn_SetBusEffect(
                   kToneBus, 0, VN_EFFECT_GAIN,
                   reinterpret_cast<const VnEffectDesc*>(&gain)),
               "Vn_SetBusEffect(tone gain)") ||
        !Check(Vn_RegisterPCM(kToneSound, &tonePcm),
               "Vn_RegisterPCM(tone)") ||
        !Check(Vn_RegisterPCM(kDialogueSound, &dialoguePcm),
               "Vn_RegisterPCM(dialogue)")) {
        Vn_Term();
        return 3;
    }

    VnBindingDesc voiceGain{};
    voiceGain.structSize = sizeof(VnBindingDesc);
    voiceGain.variableId = kStepVariable;
    voiceGain.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    voiceGain.targetId = 0;
    voiceGain.curve.structSize = sizeof(VnCurveDesc);
    voiceGain.curve.pointCount = 2;
    voiceGain.curve.points[0] = {0.0f, -18.0f};
    voiceGain.curve.points[1] = {4.0f, 0.0f};
    voiceGain.curve.handles[0] = {0.0f, -18.0f, 4.0f, 0.0f};
    voiceGain.transitionMs = 0;
    VnBindingDesc busGain{};
    busGain.structSize = sizeof(VnBindingDesc);
    busGain.variableId = kSmoothVariable;
    busGain.targetScopeKind = VN_TARGET_SCOPE_BUS;
    busGain.targetId = 0;
    busGain.curve.structSize = sizeof(VnCurveDesc);
    busGain.curve.pointCount = 2;
    busGain.curve.points[0] = {-24.0f, -24.0f};
    busGain.curve.points[1] = {0.0f, 0.0f};
    busGain.curve.handles[0] = {-24.0f, -24.0f, 0.0f, 0.0f};
    busGain.transitionMs = 40;
    VnBindingDesc effectGain{};
    effectGain.structSize = sizeof(VnBindingDesc);
    effectGain.variableId = kSmoothVariable;
    effectGain.targetScopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    effectGain.targetId = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    effectGain.curve.structSize = sizeof(VnCurveDesc);
    effectGain.curve.pointCount = 2;
    effectGain.curve.points[0] = {-24.0f, -12.0f};
    effectGain.curve.points[1] = {0.0f, 0.0f};
    effectGain.curve.handles[0] = {-24.0f, -12.0f, 0.0f, 0.0f};
    effectGain.transitionMs = 0;
    VnBindingAttachDesc voiceAttach{};
    voiceAttach.structSize = sizeof(VnBindingAttachDesc);
    voiceAttach.scopeKind = VN_TARGET_SCOPE_VOICE;
    voiceAttach.objectId = kToneSound;
    VnBindingAttachDesc busAttach{};
    busAttach.structSize = sizeof(VnBindingAttachDesc);
    busAttach.scopeKind = VN_TARGET_SCOPE_BUS;
    busAttach.objectId = kToneBus;
    VnBindingAttachDesc effectAttach{};
    effectAttach.structSize = sizeof(VnBindingAttachDesc);
    effectAttach.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    effectAttach.objectId = kToneBus;
    if (!Check(Vn_RegisterBinding(kVoiceGainBinding, &voiceGain),
               "Vn_RegisterBinding(voice gain)") ||
        !Check(Vn_RegisterBinding(kBusGainBinding, &busGain),
               "Vn_RegisterBinding(bus gain)") ||
        !Check(Vn_RegisterBinding(kEffectGainBinding, &effectGain),
               "Vn_RegisterBinding(effect gain)") ||
        !Check(Vn_AttachBinding(kVoiceGainBinding, &voiceAttach),
               "Vn_AttachBinding(voice)") ||
        !Check(Vn_AttachBinding(kBusGainBinding, &busAttach),
               "Vn_AttachBinding(bus)") ||
        !Check(Vn_AttachBinding(kEffectGainBinding, &effectAttach),
               "Vn_AttachBinding(effect)")) {
        Vn_Term();
        return 3;
    }

    std::uint32_t probeGeneration = 0;
    const harken::CurvePoint sidechainPoints[3] = {
        {-96.0f, 0.0f}, {-30.0f, 0.0f}, {-12.0f, -14.0f}};
    const harken::CurveHandlePair sidechainHandles[2] = {
        {-96.0f, 0.0f, -30.0f, 0.0f}, {-30.0f, 0.0f, -12.0f, -14.0f}};
    const std::uint32_t toneBusIndex = engine->buses.FindIndex(kToneBus);
    if (!Check(harken::InstallMeterProbe(*engine, kSidechainProbe,
                                        kDialogueBus,
                                        VN_BUS_METER_TAP_POST_EFFECTS,
                                        /*attackMs=*/10, /*releaseMs=*/220,
                                        &probeGeneration),
               "InstallMeterProbe") ||
        !Check(harken::DefineMeterProbeBinding(
                   *engine, kSidechainBinding, kSidechainProbe,
                   probeGeneration, VN_TARGET_SCOPE_BUS, /*targetId=*/0,
                   sidechainPoints, sidechainHandles, 3,
                   /*transitionMs=*/60),
               "DefineMeterProbeBinding") ||
        !Check(harken::AttachMeterProbeBinding(
                   *engine, kSidechainBinding, VN_TARGET_SCOPE_BUS,
                   kToneBus, 0, engine->buses.GenerationAt(toneBusIndex)),
               "AttachMeterProbeBinding")) {
        Vn_Term();
        return 3;
    }

    VnEmitterHandle emitter = 0;
    if (!Check(Vn_CreateEmitter(&emitter), "Vn_CreateEmitter")) {
        Vn_Term();
        return 3;
    }

    // Refusal spot checks against the installed definitions: a Step
    // fraction refuses, an out-of-range Smooth value refuses, and an
    // unknown Variable refuses without creating state.
    if (Vn_SetValue(kStepVariable, 2.5f) != VN_INVALID_ARG ||
        Vn_SetValue(kSmoothVariable, 6.0f) != VN_INVALID_ARG ||
        Vn_SetValue({0x76656e7573686f73, 0xDEAD}, 1.0f) != VN_NO_VARIABLE) {
        std::fprintf(stderr,
                     "error: installed-definition refusal matrix failed\n");
        Vn_Term();
        return 3;
    }
    if (!Check(Vn_SetValue(kStepVariable, 4.0f), "Vn_SetValue(step)") ||
        !Check(Vn_SetValue(kSmoothVariable, -6.0f), "Vn_SetValue(smooth)") ||
        !Check(Vn_SetEmitterValue(emitter, kSmoothVariable, -12.0f),
               "Vn_SetEmitterValue(smooth)") ||
        !Check(Vn_SetValue(kSmoothVariable, -6.0f),
               "Vn_SetValue(global replacement)") ||
        !Check(Vn_SetValue(kSmoothVariable, -6.0f),
               "Vn_SetValue(repeated global replacement)")) {
        Vn_Term();
        return 3;
    }
    for (std::uint32_t step = 0;
         step < 32 && engine->variables.PendingCount() != 0; ++step) {
        if (!Check(Vn_Update(), "Vn_Update(variable settle)")) {
            Vn_Term();
            return 3;
        }
        engine->nullBackend.Pump(1);
    }
    if (!Check(Vn_Update(), "Vn_Update(variable settle final)") ||
        engine->variables.PendingCount() != 0) {
        std::fprintf(stderr,
                     "error: variable writes did not settle before play\n");
        Vn_Term();
        return 3;
    }

    VnPlayDesc tonePlay{};
    tonePlay.structSize = sizeof(VnPlayDesc);
    tonePlay.soundId = kToneSound;
    tonePlay.busId = kToneBus;
    tonePlay.loopCount = 0;
    VnVoiceHandle toneVoice = 0;
    if (!Check(harken::detail::PlayVoiceRaw(&tonePlay, &toneVoice),
               "PlayVoiceRaw(tone)")) {
        Vn_Term();
        return 3;
    }
    VnPlayDesc dialoguePlay{};
    dialoguePlay.structSize = sizeof(VnPlayDesc);
    dialoguePlay.soundId = kDialogueSound;
    dialoguePlay.busId = kDialogueBus;
    dialoguePlay.loopCount = 1;

    DemoClock clock;
    clock.nullMode = true;
    clock.engine = engine;
    std::error_code directoryError;
    std::filesystem::create_directories(options.outDir, directoryError);
    const std::filesystem::path capturePath =
        std::filesystem::path(options.outDir) / "variable-world-run.f32";
    std::ofstream capture(capturePath, std::ios::binary | std::ios::trunc);
    if (directoryError || !capture.good()) {
        std::fprintf(stderr, "error: cannot create capture '%s'\n",
                     capturePath.string().c_str());
        Vn_Term();
        return 3;
    }
    std::uint64_t lastCapturedBlock = engine->mixer.BlocksRendered();
    const auto captureLatestBlock = [&]() {
        const std::uint64_t rendered = engine->mixer.BlocksRendered();
        if (rendered == lastCapturedBlock) {
            return;
        }
        const std::size_t samples =
            static_cast<std::size_t>(engine->config.blockFrames) *
            engine->nullBackend.Channels();
        capture.write(
            reinterpret_cast<const char*>(engine->nullBackend.LastBlock()),
            static_cast<std::streamsize>(samples * sizeof(float)));
        lastCapturedBlock = rendered;
    };
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    float maxMasterPeak = 0.0f;
    float maxTonePeakBeforeDialogue = 0.0f;
    float minTonePeakDuringDialogue = 1.0e9f;
    VnVoiceHandle dialogueVoice = 0;
    const int totalTicks = static_cast<int>(options.seconds / kTickSeconds);
    const int dialogueStartTick = totalTicks / 2;
    int exitCode = 0;
    for (int tick = 0; tick < totalTicks; ++tick) {
        if (tick == dialogueStartTick &&
            !Check(harken::detail::PlayVoiceRaw(&dialoguePlay, &dialogueVoice),
                   "PlayVoiceRaw(dialogue)")) {
            exitCode = 3;
            break;
        }
        if (!Check(Vn_Update(), "Vn_Update")) {
            exitCode = 3;
            break;
        }
        if (Vn_CaptureSnapshot(&snapshot) == VN_OK) {
            maxMasterPeak = std::max(maxMasterPeak, snapshot.busPeaks[0]);
            const float tonePeak = snapshot.busPeaks[toneBusIndex];
            if (tick < dialogueStartTick) {
                maxTonePeakBeforeDialogue =
                    std::max(maxTonePeakBeforeDialogue, tonePeak);
            } else if (tick > dialogueStartTick + 40 &&
                       tick < dialogueStartTick + 200) {
                minTonePeakDuringDialogue =
                    std::min(minTonePeakDuringDialogue, tonePeak);
            }
        }
        clock.Advance();
        captureLatestBlock();
    }

    (void)Vn_Stop(toneVoice, 120);
    if (dialogueVoice != 0) {
        (void)Vn_Stop(dialogueVoice, 40);
    }
    for (int tick = 0; tick < 40 && exitCode == 0; ++tick) {
        if (!Check(Vn_Update(), "Vn_Update(drain)")) {
            exitCode = 3;
            break;
        }
        clock.Advance();
        captureLatestBlock();
    }
    capture.flush();
    std::error_code captureSizeError;
    const std::uintmax_t captureBytes =
        std::filesystem::file_size(capturePath, captureSizeError);
    const bool captureWritten =
        capture.good() && !captureSizeError && captureBytes > 0;
    capture.close();
    const std::uint64_t accepted = engine->variables.AcceptedCount();
    const std::uint64_t applied = engine->variables.AppliedCount();
    float globalSmooth = 0.0f;
    float emitterSmooth = 0.0f;
    const bool fallbackObserved =
        engine->variables.CurrentValue(kSmoothVariable, &globalSmooth) &&
        engine->variables.CurrentValue(emitter, kSmoothVariable,
                                       &emitterSmooth) &&
        globalSmooth == -6.0f && emitterSmooth == globalSmooth;
    const float committedEffectGain = engine->effectParamTargets.LastCommitted(
        harken::EffectParamTargetStore::Scope::Bus, toneBusIndex, 0,
        harken::kGainParamGainDb);
    Vn_Term();

    const bool sidechainDipped =
        maxTonePeakBeforeDialogue > kSilenceFloor &&
        minTonePeakDuringDialogue <
            maxTonePeakBeforeDialogue * 0.7f;
    std::printf(
        "variable-world-run done: accepted=%llu applied=%llu "
        "maxMasterPeak=%.5f tonePre=%.5f toneDuringMin=%.5f "
        "effectGain=%.3f fallback=%s capture=%llu:%s%s%s\n",
        static_cast<unsigned long long>(accepted),
        static_cast<unsigned long long>(applied), maxMasterPeak,
        maxTonePeakBeforeDialogue, minTonePeakDuringDialogue,
        committedEffectGain, fallbackObserved ? "global" : "missing",
        static_cast<unsigned long long>(captureBytes),
        capturePath.string().c_str(),
        maxMasterPeak > kSilenceFloor ? " non-silent" : " silent",
        sidechainDipped ? " sidechain-dip" : " sidechain-flat");
    if (exitCode != 0) {
        return exitCode;
    }
    if (!captureWritten) {
        std::fprintf(stderr,
                     "error: variable-world-run capture is empty or unwritable\n");
        return 2;
    }
    if (maxMasterPeak <= kSilenceFloor) {
        std::fprintf(stderr,
                     "SILENT: variable-world-run master peak never exceeded "
                     "%g\n",
                     static_cast<double>(kSilenceFloor));
        return 2;
    }
    if (applied == 0) {
        std::fprintf(stderr,
                     "error: no accepted Variable write reached Applied\n");
        return 2;
    }
    if (!fallbackObserved) {
        std::fprintf(stderr,
                     "error: emitter Clear did not restore the Global value\n");
        return 2;
    }
    if (std::fabs(committedEffectGain - -3.0f) > 0.05f) {
        std::fprintf(stderr,
                     "error: effect Binding did not commit its authored gain "
                     "(observed %.3f)\n",
                     committedEffectGain);
        return 2;
    }
    if (!sidechainDipped) {
        std::fprintf(stderr,
                     "error: private MeterProbe sidechain produced no "
                     "observable dip\n");
        return 2;
    }
    return 0;
}

// A private MeterProbe observes the dialogue bus and maps its smoothed dBFS
// level through an internal probe Binding onto the music bus volume. The host
// only schedules dialogue bursts; the engine's meter transport, curve
// evaluation, and bus ramp produce the audible attenuation and recovery.
int RunM6Scene(const Options& options) {
    constexpr VnSoundId kMusicSound{0x76656e7573686f73, 9601};
    constexpr VnSoundId kDialogueSound{0x76656e7573686f73, 9602};
    constexpr VnBusId kMusicBus{0x76656e7573686f73, 61};
    constexpr VnBusId kDialogueBus{0x76656e7573686f73, 62};
    constexpr VnVariableId kDialogueMeter{0x76656e7573686f73, 7601};
    constexpr VnBindingId kSidechainBinding{0x76656e7573686f73, 7602};
    constexpr int kDialogueOnTicks = 300;
    constexpr int kDialogueCycleTicks = 600;

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.sampleRate = 48000;
    config.blockFrames = 256;
    config.deviceMode = options.nullMode ? VN_DEVICE_NULL : VN_DEVICE_REAL;
    config.maxVoices = 8;
    config.maxBuses = 4;
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    if (!Check(Vn_Init(&config), "Vn_Init")) {
        return 3;
    }
    std::printf("m6-scene: device is %s, %.2fs of meter-reactive sidechain attenuation\n",
                options.nullMode ? "null (headless smoke)"
                                 : "real (WASAPI shared mode)",
                options.seconds);
#if defined(_DEBUG) && defined(_MSC_VER)
    if (!harken::memguard::IsArmed()) {
        std::fprintf(stderr, "error: memguard is not armed\n");
        Vn_Term();
        return 3;
    }
#endif

    ScenePcmSource music = MakeM6MusicBed();
    ScenePcmSource dialogue = MakeM6DialogueBurst();
    const VnPCMDesc musicPcm = ScenePcmDesc(music);
    const VnPCMDesc dialoguePcm = ScenePcmDesc(dialogue);
    if (!Check(Vn_CreateBus(kMusicBus, VN_BUS_MASTER),
               "Vn_CreateBus(music)") ||
        !Check(Vn_CreateBus(kDialogueBus, VN_BUS_MASTER),
               "Vn_CreateBus(dialogue)") ||
        !Check(Vn_RegisterPCM(kMusicSound, &musicPcm),
               "Vn_RegisterPCM(music)") ||
        !Check(Vn_RegisterPCM(kDialogueSound, &dialoguePcm),
               "Vn_RegisterPCM(dialogue)")) {
        Vn_Term();
        return 3;
    }

    harken::Engine* m6Engine = harken::detail::ActiveEngine();
    std::uint32_t probeGeneration = 0;
    if (!Check(harken::InstallMeterProbe(*m6Engine, kDialogueMeter,
                                        kDialogueBus,
                                        VN_BUS_METER_TAP_POST_EFFECTS,
                                        /*attackMs=*/10, /*releaseMs=*/250,
                                        &probeGeneration),
               "InstallMeterProbe(dialogue)")) {
        Vn_Term();
        return 3;
    }
    const harken::CurvePoint sidechainPoints[3] = {
        {-96.0f, 0.0f}, {-24.0f, 0.0f}, {-12.0f, -12.0f}};
    const harken::CurveHandlePair sidechainHandles[2] = {
        {-96.0f, 0.0f, -24.0f, 0.0f}, {-24.0f, 0.0f, -12.0f, -12.0f}};
    if (!Check(harken::DefineMeterProbeBinding(
                   *m6Engine, kSidechainBinding, kDialogueMeter,
                   probeGeneration, VN_TARGET_SCOPE_BUS, /*targetId=*/0,
                   sidechainPoints, sidechainHandles, 3,
                   /*transitionMs=*/80),
               "DefineMeterProbeBinding(sidechain)")) {
        Vn_Term();
        return 3;
    }
    const std::uint32_t musicBusIndex =
        m6Engine->buses.FindIndex(kMusicBus);
    if (!Check(harken::AttachMeterProbeBinding(
                   *m6Engine, kSidechainBinding, VN_TARGET_SCOPE_BUS,
                   kMusicBus, 0, m6Engine->buses.GenerationAt(musicBusIndex)),
               "AttachMeterProbeBinding(sidechain -> music)") ||
        !Check(Vn_SetBusVolume(kMusicBus, 0.0f, 0),
               "Vn_SetBusVolume(music, initial)")) {
        Vn_Term();
        return 3;
    }

    VnPlayDesc musicPlay{};
    musicPlay.structSize = sizeof(VnPlayDesc);
    musicPlay.soundId = kMusicSound;
    musicPlay.busId = kMusicBus;
    musicPlay.loopCount = 0;
    VnVoiceHandle musicVoice = 0;
    if (!Check(harken::detail::PlayVoiceRaw(&musicPlay, &musicVoice),
               "PlayVoiceRaw(music)")) {
        Vn_Term();
        return 3;
    }

    VnPlayDesc dialoguePlay{};
    dialoguePlay.structSize = sizeof(VnPlayDesc);
    dialoguePlay.soundId = kDialogueSound;
    dialoguePlay.busId = kDialogueBus;
    dialoguePlay.loopCount = 1;

    DemoClock clock;
    clock.nullMode = options.nullMode;
    clock.engine = harken::detail::ActiveEngine();
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    float maxMasterPeak = 0.0f;
    VnVoiceHandle dialogueVoice = 0;
    bool dialogueOn = false;
    int dialogueBursts = 0;
    int exitCode = 0;
    const int totalTicks =
        static_cast<int>(options.seconds / kTickSeconds);

    std::printf("[m6 0.00s] dialogue OFF -> music at full level\n");
    for (int tick = 0; tick < totalTicks; ++tick) {
        const int cycleTick = tick % kDialogueCycleTicks;
        if (cycleTick == kDialogueOnTicks) {
            if (!Check(
                    harken::detail::PlayVoiceRaw(&dialoguePlay, &dialogueVoice),
                    "PlayVoiceRaw(dialogue)")) {
                exitCode = 3;
                break;
            }
            dialogueOn = true;
            ++dialogueBursts;
            std::printf("[m6 %.2fs] dialogue ON -> music dips -12 dB\n",
                        tick * kTickSeconds);
        } else if (cycleTick == 0 && tick > 0 && dialogueOn) {
            dialogueOn = false;
            std::printf(
                "[m6 %.2fs] dialogue OFF -> music recovers smoothly\n",
                tick * kTickSeconds);
        }

        if (!Check(Vn_Update(), "Vn_Update")) {
            exitCode = 3;
            break;
        }
        if (Vn_CaptureSnapshot(&snapshot) == VN_OK &&
            snapshot.busPeaks[0] > maxMasterPeak) {
            maxMasterPeak = snapshot.busPeaks[0];
        }
        clock.Advance();
    }

    if (dialogueOn) {
        std::printf("[m6 %.2fs] dialogue OFF -> scene ending\n",
                    totalTicks * kTickSeconds);
        (void)Vn_Stop(dialogueVoice, 20);
    }
    (void)Vn_Stop(musicVoice, 250);
    for (int tick = 0; tick < 40 && exitCode == 0; ++tick) {
        if (!Check(Vn_Update(), "Vn_Update(drain)")) {
            exitCode = 3;
            break;
        }
        if (Vn_CaptureSnapshot(&snapshot) == VN_OK &&
            snapshot.busPeaks[0] > maxMasterPeak) {
            maxMasterPeak = snapshot.busPeaks[0];
        }
        clock.Advance();
    }
    Vn_Term();

    std::printf("m6-scene done: dialogueBursts=%d maxMasterPeak=%.5f%s\n",
                dialogueBursts, maxMasterPeak,
                maxMasterPeak > kSilenceFloor ? " non-silent" : " silent");
    if (exitCode != 0) {
        return exitCode;
    }
    if (maxMasterPeak <= kSilenceFloor) {
        std::fprintf(stderr,
                     "SILENT: m6-scene master peak never exceeded %g "
                     "(max %.6g)\n",
                     static_cast<double>(kSilenceFloor),
                     static_cast<double>(maxMasterPeak));
        return 2;
    }
    return 0;
}

int RunM2Scene(const Options& options, char** argv) {
    std::error_code ec;
    std::filesystem::create_directories(".temp", ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot create .temp/: %s\n",
                     ec.message().c_str());
        return 1;
    }

    constexpr VnSoundId kMusicId{0x76656e7573686f73, 8101};
    constexpr VnSoundId kAmbienceId{0x76656e7573686f73, 8102};
    constexpr VnSoundId kOneShotId{0x76656e7573686f73, 8103};
    const std::filesystem::path musicWavPath =
        options.musicPath.empty()
            ? std::filesystem::path(".temp/harken_host_m2_music.wav")
            : std::filesystem::path(options.musicPath);
    const std::filesystem::path manifestPath =
        ".temp/harken_host_m2_scene_manifest.json";
    const std::filesystem::path streamPath =
        ".temp/harken_host_m2_scene.stream";
    const double oneShotStartSeconds =
        options.seconds * kSceneOneShotStartRatio;
    const double minMusicSeconds =
        oneShotStartSeconds + kSceneMusicMinOverlapSeconds;
    const double maxMusicSeconds =
        options.seconds + kSceneMusicMaxTailSeconds;
    double expectedMusicSeconds = options.seconds;

    if (options.musicPath.empty()) {
        const std::vector<float> music =
            MakeSceneMusic(options.seconds, 48000);
        if (!harkenhost::WriteWav16(musicWavPath.string(), music, 2, 48000)) {
            std::fprintf(stderr, "error: cannot write synthesized music wav\n");
            return 1;
        }
        std::printf("synth: wrote streamed music %s (stereo arpeggio)\n",
                    musicWavPath.string().c_str());
    } else {
        LoadedWav musicProbe;
        if (!LoadWav(options.musicPath, &musicProbe)) {
            return 1;
        }
        expectedMusicSeconds =
            static_cast<double>(musicProbe.frames) /
            static_cast<double>(musicProbe.sampleRate);
        if (expectedMusicSeconds < minMusicSeconds ||
            expectedMusicSeconds > maxMusicSeconds) {
            std::fprintf(stderr,
                         "error: --music duration %.2f s is outside "
                         "the acceptable range [%.2f, %.2f] s for this "
                         "scene\n",
                         expectedMusicSeconds, minMusicSeconds,
                         maxMusicSeconds);
            return 1;
        }
        std::printf("loaded music override: %s (%u ch, %u Hz, %llu frames, "
                    "%.2f s)\n",
                    options.musicPath.c_str(), musicProbe.channels,
                    musicProbe.sampleRate,
                    static_cast<unsigned long long>(musicProbe.frames),
                    expectedMusicSeconds);
    }

    const std::string manifest = StreamManifest(kMusicId, musicWavPath, streamPath, options.chunkFrames);
    if (!WriteTextFile(manifestPath, manifest)) {
        std::fprintf(stderr, "error: cannot write m2-scene manifest\n");
        return 1;
    }
    const std::filesystem::path packc = ResolvePackcPath(options, argv);
    if (!RunPackcStream(packc, manifestPath)) {
        std::fprintf(stderr, "error: packc --stream failed\n");
        return 1;
    }

    ScenePcmSource ambience =
        options.ambiencePath.empty() ? MakeSceneAmbience() : ScenePcmSource{};
    if (!options.ambiencePath.empty() &&
        !LoadScenePcmSource(options.ambiencePath, "ambience", &ambience)) {
        return 1;
    }
    ScenePcmSource oneShot =
        options.oneshotPath.empty() ? MakeSceneOneShot() : ScenePcmSource{};
    if (!options.oneshotPath.empty() &&
        !LoadScenePcmSource(options.oneshotPath, "one-shot", &oneShot)) {
        return 1;
    }
    if (options.ambiencePath.empty()) {
        std::printf("synth: in-memory ambience is %.2fs, %u channels\n",
                    static_cast<double>(ambience.frames) /
                        static_cast<double>(ambience.sampleRate),
                    ambience.channels);
    }
    if (options.oneshotPath.empty()) {
        std::printf("synth: in-memory one-shot is %.2fs, %u channel\n",
                    static_cast<double>(oneShot.frames) /
                        static_cast<double>(oneShot.sampleRate),
                    oneShot.channels);
    }

    SceneNotificationLog notifications;
    constexpr std::uint32_t kStreamRingFrames = 16384;
    constexpr std::uint32_t kMaxStreams = 1;
    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode =
        options.nullMode ? VN_DEVICE_NULL : VN_DEVICE_REAL;
    config.fxSlotsPerVoice = 1;
    config.delayEffectPoolCapacity = 2;
    config.streamRingFrames = kStreamRingFrames;
    config.maxStreams = kMaxStreams;
    config.onNotification = &RecordSceneNotification;
    config.notificationUserData = &notifications;
    if (!Check(Vn_Init(&config), "Vn_Init")) {
        return 3;
    }
#if defined(_DEBUG) && defined(_MSC_VER)
    if (!harken::memguard::IsArmed()) {
        std::fprintf(stderr, "error: memguard is not armed\n");
        Vn_Term();
        return 3;
    }
#endif

    const VnPCMDesc ambienceDesc = ScenePcmDesc(ambience);
    const VnPCMDesc oneShotDesc = ScenePcmDesc(oneShot);
    if (!Check(Vn_RegisterStreamFile(kMusicId,
                                     streamPath.string().c_str()),
               "Vn_RegisterStreamFile(music)") ||
        !Check(Vn_RegisterPCM(kAmbienceId, &ambienceDesc),
               "Vn_RegisterPCM(ambience)") ||
        !Check(Vn_RegisterPCM(kOneShotId, &oneShotDesc),
               "Vn_RegisterPCM(one-shot)")) {
        Vn_Term();
        return 3;
    }

    VnPlayDesc playMusic{};
    playMusic.structSize = sizeof(VnPlayDesc);
    playMusic.soundId = kMusicId;
    playMusic.busId = VN_BUS_MASTER;
    playMusic.loopCount = 1;
    playMusic.volumeDb = -9.0f;

    VnPlayDesc playAmbience{};
    playAmbience.structSize = sizeof(VnPlayDesc);
    playAmbience.soundId = kAmbienceId;
    playAmbience.busId = VN_BUS_MASTER;
    playAmbience.loopCount = 0;
    playAmbience.volumeDb = -13.0f;

    VnPlayDesc playOneShot{};
    playOneShot.structSize = sizeof(VnPlayDesc);
    playOneShot.soundId = kOneShotId;
    playOneShot.busId = VN_BUS_MASTER;
    playOneShot.loopCount = 1;
    playOneShot.volumeDb = -1.0f;

    // Music is moderate, ambience is still audible under it, and the
    // delayed one-shot is deliberately prominent: -9 dB, -13 dB, and -1 dB
    // keep the three authored source peaks independently hearable without
    // relying on a master limiter.
    if (!Check(harken::detail::PlayVoiceRaw(&playMusic, &notifications.music),
               "PlayVoiceRaw(music)") ||
        !Check(
            harken::detail::PlayVoiceRaw(&playAmbience, &notifications.ambience),
            "PlayVoiceRaw(ambience)")) {
        Vn_Term();
        return 3;
    }
    const auto playStarted = std::chrono::steady_clock::now();

    DemoClock clock;
    clock.nullMode = options.nullMode;
    clock.engine = harken::detail::ActiveEngine();

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    float maxPeak = 0.0f;
    float maxConcurrentPeak = 0.0f;
    float maxConcurrentMusicPeak = 0.0f;
    float maxConcurrentAmbiencePeak = 0.0f;
    float maxConcurrentOneShotPeak = 0.0f;
    int concurrentSnapshots = 0;
    bool oneShotStarted = false;
    bool ambienceFadeStarted = false;
    const int sceneTicks =
        static_cast<int>(options.seconds / kTickSeconds);
    const int oneShotTick =
        static_cast<int>(oneShotStartSeconds / kTickSeconds);
    const int ambienceFadeTick = static_cast<int>(
        (options.seconds - 1.2) / kTickSeconds);
    const int maxTicks =
        static_cast<int>((options.seconds + kSceneMusicMaxTailSeconds) /
                         kTickSeconds);

    std::printf("m2-scene: %.2fs, one-shot at %.2fs, ambience fade %.2fs\n",
                options.seconds, oneShotTick * kTickSeconds,
                ambienceFadeTick * kTickSeconds);

    int tick = 0;
    int exitCode = 0;
    for (; tick < maxTicks; ++tick) {
        if (tick == oneShotTick) {
            if (!Check(
                    harken::detail::PlayVoiceRaw(&playOneShot,
                                                &notifications.oneShot),
                    "PlayVoiceRaw(one-shot)")) {
                exitCode = 3;
                break;
            }
            VnFeedbackDelayDesc delay{};
            delay.structSize = sizeof(VnFeedbackDelayDesc);
            delay.delayMs = 350.0f;
            delay.feedback = 0.45f;
            delay.wet = 0.85f;
            delay.dry = 1.0f;
            if (!Check(Vn_SetVoiceEffect(notifications.oneShot, 0,
                                         VN_EFFECT_FEEDBACK_DELAY,
                                         EffectBase(&delay)),
                       "Vn_SetVoiceEffect(one-shot delay)")) {
                exitCode = 3;
                break;
            }
            oneShotStarted = true;
            std::printf("[m2 %.2fs] delayed one-shot triggered\n",
                        tick * kTickSeconds);
        }
        if (tick == ambienceFadeTick) {
            if (!Check(Vn_Stop(notifications.ambience, 1200),
                       "Vn_Stop(ambience fade)")) {
                exitCode = 3;
                break;
            }
            ambienceFadeStarted = true;
            std::printf("[m2 %.2fs] ambience fade-stop started\n",
                        tick * kTickSeconds);
        }
        notifications.currentTick = tick;
        notifications.currentElapsedSeconds =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - playStarted)
                .count();
        if (!Check(Vn_Update(), "Vn_Update")) {
            exitCode = 3;
            break;
        }
        if (Vn_CaptureSnapshot(&snapshot) == VN_OK) {
            if (snapshot.busPeaks[0] > maxPeak) {
                maxPeak = snapshot.busPeaks[0];
            }
            const bool concurrent =
                oneShotStarted && SceneVoiceLive(snapshot, notifications.music) &&
                SceneVoiceLive(snapshot, notifications.ambience) &&
                SceneVoiceLive(snapshot, notifications.oneShot) &&
                snapshot.activeVoiceCount >= 3;
            if (concurrent) {
                ++concurrentSnapshots;
                if (snapshot.busPeaks[0] > maxConcurrentPeak) {
                    maxConcurrentPeak = snapshot.busPeaks[0];
                }
                const float musicPeak =
                    SceneVoiceTapPostGain(snapshot, notifications.music);
                const float ambiencePeak =
                    SceneVoiceTapPostGain(snapshot, notifications.ambience);
                const float oneShotPeak =
                    SceneVoiceTapPostGain(snapshot, notifications.oneShot);
                if (musicPeak > maxConcurrentMusicPeak) {
                    maxConcurrentMusicPeak = musicPeak;
                }
                if (ambiencePeak > maxConcurrentAmbiencePeak) {
                    maxConcurrentAmbiencePeak = ambiencePeak;
                }
                if (oneShotPeak > maxConcurrentOneShotPeak) {
                    maxConcurrentOneShotPeak = oneShotPeak;
                }
            }
            if (tick % kPrintEveryTicks == 0) {
                std::printf("[m2 %.2fs] voices=%u peak=%.5f "
                            "streamHW=%llu starved=%llu\n",
                            tick * kTickSeconds,
                            snapshot.activeVoiceCount,
                            snapshot.busPeaks[0],
                            static_cast<unsigned long long>(
                                snapshot.streamHighWater),
                            static_cast<unsigned long long>(
                                snapshot.voiceStarvedCount));
            }
        }
        clock.Advance();
        if (tick >= sceneTicks && notifications.musicEnded &&
            notifications.oneShotEnded && notifications.ambienceStoppedByFade) {
            break;
        }
    }
    notifications.currentTick = tick;
    notifications.currentElapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      playStarted)
            .count();
    (void)Vn_Update();
    (void)Vn_CaptureSnapshot(&snapshot);
    const std::uint64_t guardHits = harken::memguard::HitCount();
    Vn_Term();

    const std::uint64_t roundedRingFrames =
        RoundUpPow2Host(kStreamRingFrames);
    std::printf("m2-scene summary: musicEnded=%s oneShotEnded=%s "
                "ambienceFadeDone=%s fadeStarted=%s "
                "musicEndTick=%d musicEndElapsed=%.2f "
                "concurrentSnapshots=%d maxPeak=%.5f maxConcurrentPeak=%.5f "
                "musicPeak=%.5f ambiencePeak=%.5f oneShotPeak=%.5f "
                "streamHighWater=%llu ringHighWater=%llu "
                "voiceStarved=%llu eventStarved=%llu memguardHits=%llu "
                "ticks=%d\n",
                notifications.musicEnded ? "yes" : "no",
                notifications.oneShotEnded ? "yes" : "no",
                notifications.ambienceStoppedByFade ? "yes" : "no",
                ambienceFadeStarted ? "yes" : "no", notifications.musicEndTick,
                notifications.musicEndElapsedSeconds, concurrentSnapshots, maxPeak,
                maxConcurrentPeak, maxConcurrentMusicPeak,
                maxConcurrentAmbiencePeak, maxConcurrentOneShotPeak,
                static_cast<unsigned long long>(snapshot.streamHighWater),
                static_cast<unsigned long long>(
                    snapshot.streamRingHighWaterFrames),
                static_cast<unsigned long long>(snapshot.voiceStarvedCount),
                static_cast<unsigned long long>(notifications.starved),
                static_cast<unsigned long long>(guardHits), tick);

    if (exitCode != 0) {
        return exitCode;
    }
    if (maxPeak < kSilenceFloor) {
        std::fprintf(stderr,
                     "SILENT: m2-scene master peak stayed below %g "
                     "(max %.6g)\n",
                     static_cast<double>(kSilenceFloor),
                     static_cast<double>(maxPeak));
        return 2;
    }
    if (concurrentSnapshots < 3) {
        std::fprintf(stderr,
                     "error: m2-scene observed only %d concurrent snapshots "
                     "with streamed music, looping ambience, and one-shot "
                     "voices live\n",
                     concurrentSnapshots);
        return 3;
    }
    if (maxConcurrentPeak < kSilenceFloor) {
        std::fprintf(stderr,
                     "SILENT: m2-scene concurrent-window peak stayed below "
                     "%g (max %.6g)\n",
                     static_cast<double>(kSilenceFloor),
                     static_cast<double>(maxConcurrentPeak));
        return 2;
    }
#if HARKEN_STAGE_TAPS_ACTIVE
    // Per-element evidence reads the per-voice taps, measured only when
    // stage taps are compiled in (Debug, or the explicit opt-in). The
    // master and concurrent-window verdicts above read bus peaks and hold
    // in every build (the bus output scan is unconditional from M6).
    bool elementSilent = false;
    if (maxConcurrentMusicPeak < kSilenceFloor) {
        std::fprintf(stderr,
                     "SILENT: music voice tap never exceeded %g during "
                     "the concurrent window (max %.6g)\n",
                     static_cast<double>(kSilenceFloor),
                     static_cast<double>(maxConcurrentMusicPeak));
        elementSilent = true;
    }
    if (maxConcurrentAmbiencePeak < kSilenceFloor) {
        std::fprintf(stderr,
                     "SILENT: ambience voice tap never exceeded %g during "
                     "the concurrent window (max %.6g)\n",
                     static_cast<double>(kSilenceFloor),
                     static_cast<double>(maxConcurrentAmbiencePeak));
        elementSilent = true;
    }
    if (maxConcurrentOneShotPeak < kSilenceFloor) {
        std::fprintf(stderr,
                     "SILENT: one-shot voice tap never exceeded %g during "
                     "the concurrent window (max %.6g)\n",
                     static_cast<double>(kSilenceFloor),
                     static_cast<double>(maxConcurrentOneShotPeak));
        elementSilent = true;
    }
    if (elementSilent) {
        return 2;
    }
#else
    std::printf("note: per-voice tap evidence unavailable in this build "
                "(stage taps compiled out); the bus-peak verdicts above "
                "stand alone\n");
#endif
    if (!notifications.musicEnded || !notifications.oneShotEnded ||
        !notifications.ambienceStoppedByFade) {
        std::fprintf(stderr,
                     "error: m2-scene did not complete naturally "
                     "(musicEnded=%s oneShotEnded=%s ambienceFadeDone=%s)\n",
                     notifications.musicEnded ? "yes" : "no",
                     notifications.oneShotEnded ? "yes" : "no",
                     notifications.ambienceStoppedByFade ? "yes" : "no");
        return 3;
    }
    const double earliestMusicEndSeconds =
        expectedMusicSeconds - kSceneMusicEndToleranceSeconds;
    // In null mode DemoClock is the audio clock, so tick time is exact.
    // With a real device the host tick uses sleep_for and can overshoot, so
    // tick counts under-report healthy runs; compare the notification observation
    // time in the wall-clock domain instead.
    if (options.nullMode) {
        const int earliestMusicEndTick = static_cast<int>(
            earliestMusicEndSeconds / kTickSeconds);
        if (notifications.musicEndTick >= 0 &&
            notifications.musicEndTick < earliestMusicEndTick) {
            std::fprintf(stderr,
                         "error: music ended at %.2f s, earlier than the "
                         "expected duration %.2f s minus %.2f s tolerance\n",
                         notifications.musicEndTick * kTickSeconds,
                         expectedMusicSeconds,
                         kSceneMusicEndToleranceSeconds);
            return 3;
        }
    } else if (notifications.musicEndElapsedSeconds >= 0.0 &&
               notifications.musicEndElapsedSeconds < earliestMusicEndSeconds) {
        std::fprintf(stderr,
                     "error: music ended at %.2f s wall time, earlier than "
                     "the expected duration %.2f s minus %.2f s tolerance\n",
                     notifications.musicEndElapsedSeconds, expectedMusicSeconds,
                     kSceneMusicEndToleranceSeconds);
        return 3;
    }
    if (snapshot.streamHighWater > kMaxStreams ||
        snapshot.streamRingHighWaterFrames > roundedRingFrames ||
        snapshot.voiceStarvedCount != 0 || notifications.starved != 0 ||
        guardHits != 0) {
        std::fprintf(stderr,
                     "error: m2-scene self-check counters failed "
                     "(streamHW=%llu ringHW=%llu voiceStarved=%llu "
                     "eventStarved=%llu memguardHits=%llu)\n",
                     static_cast<unsigned long long>(
                         snapshot.streamHighWater),
                     static_cast<unsigned long long>(
                         snapshot.streamRingHighWaterFrames),
                     static_cast<unsigned long long>(
                         snapshot.voiceStarvedCount),
                     static_cast<unsigned long long>(notifications.starved),
                     static_cast<unsigned long long>(guardHits));
        return 3;
    }
    return 0;
}

int RunDemo(const Options& options) {
    // --- sources ---
    std::string pathA, pathB;
    if (options.synth) {
        if (!SynthesizeSources(options.seconds, &pathA, &pathB)) {
            return 1;
        }
    } else {
        pathA = options.wavPaths[0];
        pathB = options.wavPaths[1];
    }
    LoadedWav wavA, wavB;
    if (!LoadWav(pathA, &wavA) || !LoadWav(pathB, &wavB)) {
        return 1;
    }
    std::printf("loaded: %s (%u ch, %u Hz, %llu frames)\n", pathA.c_str(),
                wavA.channels, wavA.sampleRate,
                static_cast<unsigned long long>(wavA.frames));
    std::printf("loaded: %s (%u ch, %u Hz, %llu frames)\n", pathB.c_str(),
                wavB.channels, wavB.sampleRate,
                static_cast<unsigned long long>(wavB.frames));

    // The 3D voice needs a mono emitter; a stereo B downmixes by mean.
    std::vector<float> mono3d;
    if (wavB.channels == 2) {
        std::printf(
            "note: 3D source is stereo; downmixing to mono (mean)\n");
        mono3d.resize(static_cast<std::size_t>(wavB.frames));
        for (std::size_t i = 0; i < mono3d.size(); ++i) {
            mono3d[i] =
                0.5f * (wavB.samples[i * 2] + wavB.samples[i * 2 + 1]);
        }
    } else {
        mono3d = wavB.samples;
    }

    // --- engine ---
    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode =
        options.nullMode ? VN_DEVICE_NULL : VN_DEVICE_REAL;
    if (!Check(Vn_Init(&config), "Vn_Init")) {
        return 3;
    }
    std::printf("engine: %s device\n",
                options.nullMode ? "null (headless smoke)"
                                 : "real (WASAPI shared mode)");

    int exitCode = 0;
    float maxMasterPeak = 0.0f;
    float max3dVoiceTap = 0.0f;
    {
        VnPCMDesc descA{};
        descA.structSize = sizeof(VnPCMDesc);
        descA.sampleRate = wavA.sampleRate;
        descA.channels = wavA.channels;
        descA.layout = VN_PCM_LAYOUT_INTERLEAVED;
        descA.policy = VN_PCM_POLICY_COPY;
        descA.frames = wavA.frames;
        descA.samplesInterleaved = wavA.samples.data();

        VnPCMDesc descB{};
        descB.structSize = sizeof(VnPCMDesc);
        descB.sampleRate = wavB.sampleRate;
        descB.channels = 1;
        descB.layout = VN_PCM_LAYOUT_INTERLEAVED;
        descB.policy = VN_PCM_POLICY_COPY;
        descB.frames = mono3d.size();
        descB.samplesInterleaved = mono3d.data();

        const VnAttenuationDesc profile = DemoProfile();
        VnVoiceHandle voice2d = 0;
        VnVoiceHandle voice3d = 0;

        VnPlayDesc play2d{};
        play2d.structSize = sizeof(VnPlayDesc);
        play2d.soundId = kSound2D;
        play2d.busId = VN_BUS_MASTER;
        play2d.loopCount = 1; // single pass (0 would loop forever)
        // Authored deliberately quiet: during phase A the noise sits far
        // under the flyby so the moving voice is the clear subject; a
        // voice-volume ramp brings it back for phases B and C.
        play2d.volumeDb = -24.0f;

        VnPlayDesc play3d{};
        play3d.structSize = sizeof(VnPlayDesc);
        play3d.soundId = kSound3D;
        play3d.busId = VN_BUS_MASTER;
        play3d.loopCount = 1; // single pass (0 would loop forever)
        play3d.spatialized = 1;
        // Pre-compensates the inverse-square floor of the scripted path:
        // the closest approach (distance 4) sits roughly -24 dB below
        // unity rolloff with at most a few dB of near-field boost on
        // top, so +12 dB keeps the flyby clearly audible while the
        // worst-case summed peak stays well under 1.0.
        play3d.volumeDb = 12.0f;
        const VnVec3 start = ScriptedPosition(0.0);
        play3d.positionX = start.x;
        play3d.positionY = start.y;
        play3d.positionZ = start.z;

        // The first volume ever set on a bus snaps regardless of rampMs
        // (header contract), so spend that snap here on 0 dB; the phase B
        // ramps then really ramp.
        const bool ready =
            Check(Vn_RegisterPCM(kSound2D, &descA), "Vn_RegisterPCM(2D)") &&
            Check(Vn_RegisterPCM(kSound3D, &descB), "Vn_RegisterPCM(3D)") &&
            Check(Vn_RegisterAttenuation(kProfileId, &profile),
                  "Vn_RegisterAttenuation") &&
            Check(Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0),
                  "Vn_SetBusVolume(master, initial snap)") &&
            Check(harken::detail::PlayVoiceRaw(&play2d, &voice2d),
                  "PlayVoiceRaw(2D)") &&
            Check(harken::detail::PlayVoiceRaw(&play3d, &voice3d),
                  "PlayVoiceRaw(3D)") &&
            Check(Vn_BindVoiceAttenuation(voice3d, kProfileId),
                  "Vn_BindVoiceAttenuation");
        if (!ready) {
            Vn_Term();
            return 3;
        }

        DemoClock clock;
        clock.nullMode = options.nullMode;
        clock.engine = harken::detail::ActiveEngine();

        const harken::VariableDefinitionV1 cutoffDefinition{
            kCutoffVariable, harken::VariableKindV1::Smooth,
            0.0f, 1.0f, 1.0f, {}};
        harken::ContainerWorldInstallViewV1 cutoffWorld{};
        cutoffWorld.variables = &cutoffDefinition;
        cutoffWorld.variableCount = 1;
        cutoffWorld.identity.capabilityMask =
            harken::kWorldCapabilityVariableDefinitions;
        cutoffWorld.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV5(cutoffWorld);
        if (harken::InstallContainerWorldV1(*clock.engine, cutoffWorld).status !=
            harken::WorldInstallStatus::Ok) {
            std::fprintf(stderr,
                         "error: installing the cutoff Variable failed\n");
            Vn_Term();
            return 3;
        }

        // Phase C's binding: a Variable drives the 2D voice's lowpass
        // cutoff through a linear curve (value 0 -> 500 Hz, 1 -> 20 kHz).
        // Binding assembly goes through the engine-internal surface — the
        // milestone public API drives the Variable only — mirroring the
        // golden-case mechanics. The Variable starts at 1 (cutoff fully
        // open) so phases A and B sound unchanged.
        {
            harken::Curve curve;
            const harken::CurvePoint pts[] = {{0.0f, 500.0f},
                                             {1.0f, 20000.0f}};
            const harken::CurveHandlePair handles[] = {
                {pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
            const bool bound =
                curve.SetPoints(pts, handles, 2) &&
                clock.engine->params.BindVoiceTarget(
                    kCutoffVariable, voice2d,
                    harken::kTargetVoiceLowpassCutoff, curve) &&
                Check(Vn_SetValue(kCutoffVariable, 1.0f),
                      "Vn_SetValue(cutoff, initial)");
            if (!bound) {
                std::fprintf(stderr,
                             "error: assembling the lowpass curve binding "
                             "failed\n");
                Vn_Term();
                return 3;
            }
        }

        VnSnapshot snapshot{};
        snapshot.structSize = sizeof(VnSnapshot);
        // Snapshot voice entries are indexed by slot — the handle's low
        // 32 bits — which lets the demo watch each voice specifically.
        const std::uint32_t slot2d =
            static_cast<std::uint32_t>(voice2d & 0xffffffffu);
        const std::uint32_t slot3d =
            static_cast<std::uint32_t>(voice3d & 0xffffffffu);
        VoiceStatesSeen states2d;
        VoiceStatesSeen states3d;

        const int totalTicks =
            static_cast<int>(options.seconds / kTickSeconds);
        const PhasePlan plan = PhasePlan::Make(totalTicks);
        VnVec3 position = start;
        for (int tick = 0; tick < totalTicks && exitCode == 0; ++tick) {
            const double t = tick * kTickSeconds;
            // Phase entries and the phase B ramp schedule are keyed to
            // exact ticks; each phase announces its wall-clock window so
            // a listener knows what to attend to when.
            if (tick == 0) {
                std::printf(
                    "[phase A %.2fs..%.2fs] 3D flyby — listen: moving "
                    "voice pans; +x = RIGHT ear; quieter and duller when "
                    "far, louder/fuller near center (the steady noise "
                    "voice is deliberately quiet here)\n",
                    0.0, plan.startB * kTickSeconds);
            }
            if (tick == plan.startB) {
                std::printf(
                    "[phase B %.2fs..%.2fs] steady voice returns, then "
                    "the master bus ramps to -18 dB and back — listen: "
                    "whole mix dips smoothly and returns — no "
                    "zipper/stair-step buzz\n",
                    plan.startB * kTickSeconds, plan.startC * kTickSeconds);
                // Bring the noise voice back for phases B and C: volume
                // contributions add in dB, so +18 dB API-direct on the
                // -24 dB authored volume restores the original -6 dB
                // total.
                if (!Check(Vn_SetVoiceVolume(voice2d, 18.0f, 500),
                           "Vn_SetVoiceVolume(2D, restore)")) {
                    exitCode = 3;
                    break;
                }
            }
            if (tick == plan.rampDownTick) {
                if (!Check(Vn_SetBusVolume(VN_BUS_MASTER, -18.0f,
                                           plan.rampMs),
                           "Vn_SetBusVolume(master, ramp down)")) {
                    exitCode = 3;
                    break;
                }
            }
            if (tick == plan.rampBackTick) {
                if (!Check(Vn_SetBusVolume(VN_BUS_MASTER, 0.0f,
                                           plan.rampMs),
                           "Vn_SetBusVolume(master, ramp up)")) {
                    exitCode = 3;
                    break;
                }
            }
            if (tick == plan.startC) {
                std::printf(
                    "[phase C %.2fs..%.2fs] Variable sweeps the 2D "
                    "voice's lowpass cutoff — listen: steady voice sweeps "
                    "muffled<->bright following the value\n",
                    plan.startC * kTickSeconds,
                    plan.totalTicks * kTickSeconds);
            }
            if (tick >= plan.startC) {
                // Triangle sweep 1 -> 0 -> 1 across the phase; through
                // the curve that is cutoff 20 kHz -> 500 Hz -> 20 kHz.
                // The last tick divides by ticksC - 1 so the sweep
                // lands exactly back on the 1.0 endpoint.
                const int ticksC = plan.totalTicks - plan.startC;
                const double p =
                    ticksC > 1 ? static_cast<double>(tick - plan.startC) /
                                     (ticksC - 1)
                               : 0.0;
                const float value =
                    static_cast<float>(std::fabs(1.0 - 2.0 * p));
                // Transient backpressure only delays the sweep a tick.
                (void)Vn_SetValue(kCutoffVariable, value);
            }
            position = ScriptedPosition(t);
            // A stale handle just means the source ran out early; the
            // demo keeps running on the 2D voice.
            (void)Vn_SetVoicePosition(voice3d, position);
            if (!Check(Vn_Update(), "Vn_Update")) {
                exitCode = 3;
                break;
            }
            if (Vn_CaptureSnapshot(&snapshot) == VN_OK) {
                if (snapshot.busPeaks[0] > maxMasterPeak) {
                    maxMasterPeak = snapshot.busPeaks[0];
                }
                if (slot3d < VN_SNAPSHOT_MAX_VOICES &&
                    snapshot.voices[slot3d].tapPostGain > max3dVoiceTap) {
                    max3dVoiceTap = snapshot.voices[slot3d].tapPostGain;
                }
                if (slot2d < VN_SNAPSHOT_MAX_VOICES) {
                    states2d.Observe(snapshot.voices[slot2d].state);
                }
                if (slot3d < VN_SNAPSHOT_MAX_VOICES) {
                    states3d.Observe(snapshot.voices[slot3d].state);
                }
                if (tick % kPrintEveryTicks == 0) {
                    std::printf(
                        "[t=%5.2fs] voices=%u masterPeak=%.4f "
                        "pos=(%6.1f, %5.1f)\n",
                        t, snapshot.activeVoiceCount,
                        snapshot.busPeaks[0], position.x, position.z);
                }
            }
            clock.Advance();
        }

        // Phase D: fade both voices out and keep rendering long enough
        // for the ramps to finish; the peak tracker keeps watching.
        std::printf(
            "[phase D %.2fs..] fade-stop — both voices ramp to silence "
            "over 1.2 s\n",
            plan.totalTicks * kTickSeconds);
        (void)Vn_Stop(voice2d, 1200);
        (void)Vn_Stop(voice3d, 1200);
        for (int tick = 0; tick < 150 && exitCode == 0; ++tick) {
            if (!Check(Vn_Update(), "Vn_Update")) {
                exitCode = 3;
                break;
            }
            if (Vn_CaptureSnapshot(&snapshot) == VN_OK) {
                // Sparse decay evidence: the fade should read as a
                // monotonic slide to zero across these lines.
                if (tick % 10 == 0) {
                    std::printf("[drain %02d] masterPeak=%.5f\n", tick,
                                snapshot.busPeaks[0]);
                }
                if (snapshot.busPeaks[0] > maxMasterPeak) {
                    maxMasterPeak = snapshot.busPeaks[0];
                }
                if (slot3d < VN_SNAPSHOT_MAX_VOICES &&
                    snapshot.voices[slot3d].tapPostGain > max3dVoiceTap) {
                    max3dVoiceTap = snapshot.voices[slot3d].tapPostGain;
                }
                if (slot2d < VN_SNAPSHOT_MAX_VOICES) {
                    states2d.Observe(snapshot.voices[slot2d].state);
                }
                if (slot3d < VN_SNAPSHOT_MAX_VOICES) {
                    states3d.Observe(snapshot.voices[slot3d].state);
                }
            }
            clock.Advance();
        }

        // End-of-run snapshot summary: one final capture for the current
        // facts, plus the states and peaks tracked across the whole run.
        if (exitCode == 0 && Vn_CaptureSnapshot(&snapshot) == VN_OK) {
            std::printf("snapshot summary:\n");
            states2d.Print("2D voice");
            states3d.Print("3D voice");
            std::printf("  master bus peak: final %.4f, run max %.4f\n",
                        snapshot.busPeaks[0], maxMasterPeak);
            std::printf("  voiceHighWater=%llu arenaHighWaterBytes=%llu\n",
                        static_cast<unsigned long long>(
                            snapshot.voiceHighWater),
                        static_cast<unsigned long long>(
                            snapshot.arenaHighWaterBytes));
        }
    }
    Vn_Term();

    if (exitCode != 0) {
        return exitCode;
    }
    // Non-silence self-check, two independent pieces of evidence: the
    // master bus must have carried signal (measured in every build — the
    // bus output scan is unconditional from M6), AND, when stage taps are
    // compiled in, the 3D voice itself must have produced signal (its
    // pre-pan tap) — a mute 3D voice hiding behind a healthy 2D voice is
    // a failed demo too.
    const bool masterSilent = maxMasterPeak < kSilenceFloor;
    if (masterSilent) {
        std::fprintf(stderr,
                     "SILENT: master bus peak never exceeded %g (max "
                     "%.6g) — run judged silent\n",
                     static_cast<double>(kSilenceFloor),
                     static_cast<double>(maxMasterPeak));
    }
#if HARKEN_STAGE_TAPS_ACTIVE
    const bool voice3dSilent = max3dVoiceTap < kSilenceFloor;
    if (voice3dSilent) {
        std::fprintf(stderr,
                     "SILENT: 3D voice tap peak never exceeded %g (max "
                     "%.6g) — the moving voice never sounded\n",
                     static_cast<double>(kSilenceFloor),
                     static_cast<double>(max3dVoiceTap));
    }
#else
    const bool voice3dSilent = false;
    std::printf("note: 3D voice tap evidence unavailable in this build "
                "(stage taps compiled out); the master-bus verdict stands "
                "alone\n");
#endif
    if (masterSilent || voice3dSilent) {
        return 2;
    }
    std::printf(
        "done: max master peak %.4f, 3D voice tap peak %.4f — "
        "non-silent\n",
        maxMasterPeak, max3dVoiceTap);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "sched-measure") == 0) {
        return harkenhost::RunSchedulingMeasure(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "remote-serve") == 0) {
        return harkenhost::RunRemoteServe(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "trace-replay") == 0) {
        return harkenhost::RunTraceReplay(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "downmix-null-drill") == 0) {
        return harkenhost::RunDownmixNullDrill(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "pack-play") == 0) {
        return harkenhost::RunPackPlay(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "preview-play") == 0) {
        return harkenhost::RunPreviewPlay(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "att03-keep") == 0) {
        return harkenhost::RunAtt03KeepProbe(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "att04-nearfar") == 0) {
        return harkenhost::RunAtt04NearFarProbe(argc, argv);
    }
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintUsage();
        return 1;
    }
    if (options.mode == Options::Mode::StreamEndurance) {
        return RunStreamEndurance(options, argv);
    }
    if (options.mode == Options::Mode::M2Scene) {
        return RunM2Scene(options, argv);
    }
    if (options.mode == Options::Mode::M6Scene) {
        return RunM6Scene(options);
    }
    if (options.mode == Options::Mode::M7S7Render) {
        return RunM7S7Render(options);
    }
    if (options.mode == Options::Mode::VariableWorldRun) {
        return RunVariableWorldRun(options);
    }
    return RunDemo(options);
}
