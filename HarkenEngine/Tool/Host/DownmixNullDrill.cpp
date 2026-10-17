// See DownmixNullDrill.h. Renders one deterministic multichannel program per
// layout twice on the null backend — once at device == mix (identity, no fold)
// to capture the engine's own pre-fold mix, and once at a stereo device (the
// master ITU-R BS.775 output fold) to capture the folded output — and writes
// both as raw interleaved float32. Capturing the pre-fold mix from the engine
// (rather than re-emitting the synthesized PCM) means the reference tool folds
// the byte-identical signal the engine folded, so the null comparison isolates
// the fold matrix application and is immune to any voice-start ramp or
// parameter-commit transient (both renders share it).
#include "DownmixNullDrill.h"

#include <harken/Harken.h>

#include "Engine.h" // internal: the null-backend pump (test-clock precedent)
#include "HarkenApiInternal.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace harkenhost {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBlockFrames = 256;
constexpr double kMaxDrillSeconds = 60.0;

// One drill layout: a source at `mixLayout` folded to stereo at the device.
struct DrillSpec {
    const char* name;
    std::uint32_t mixLayout;
    std::uint32_t channels; // source/mix channel count for this layout
    const char* srcLayoutName;
    const char* sourceFile;
    const char* engineFile;
};

// Program synthesis (deterministic; the reference never regenerates it — it
// folds the captured pre-fold mix). Every channel carries a distinct base sine
// so its routing is traceable; the center channel carries a voice-band
// amplitude-modulated "dialogue" segment; the LFE carries a low-frequency
// segment (dropped by the fold — proving both sides zero it); and one window
// drives every channel with an in-phase burst so the fold summation is
// stressed. Amplitudes are bounded so the fold cannot clip:
// every channel's |sample| <= 0.26 (base 0.08 + at most one feature <= 0.18),
// and the widest fold row (7.1->stereo Lo = FL + (FC+BL+SL)/sqrt2) sums to
// 1 + 3/sqrt(2) = 3.121, so a folded sample cannot exceed 0.26*3.121 = 0.81
// (< 1.0, the device clip point). Channel roles by index (Appendix A):
// 5.1/7.1 share FL0 FR1 FC2 LFE3 BL4 BR5 (7.1 adds SL6 SR7).
std::vector<float> GenerateProgram(std::uint32_t channels,
                                   std::uint64_t frames) {
    constexpr std::uint32_t kFcIndex = 2;
    constexpr std::uint32_t kLfeIndex = 3;
    constexpr double kBaseAmp = 0.08;
    constexpr double kDialogueAmp = 0.18;
    constexpr double kLfeAmp = 0.30;
    constexpr double kBurstAmp = 0.18;

    std::vector<float> program(static_cast<std::size_t>(frames) * channels,
                               0.0f);
    const double duration = static_cast<double>(frames) / kSampleRate;
    // Non-overlapping feature windows (fractions of the run); the dialogue and
    // LFE windows overlap in time but sit on different channels.
    const double dialogueStart = 0.15 * duration;
    const double dialogueEnd = 0.70 * duration;
    const double lfeStart = 0.20 * duration;
    const double lfeEnd = 0.40 * duration;
    const double burstStart = 0.75 * duration;
    const double burstEnd = 0.82 * duration;

    for (std::uint64_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / kSampleRate;
        const bool inDialogue = t >= dialogueStart && t < dialogueEnd;
        const bool inLfe = t >= lfeStart && t < lfeEnd;
        const bool inBurst = t >= burstStart && t < burstEnd;
        const double burst =
            inBurst ? kBurstAmp * std::sin(2.0 * kPi * 300.0 * t) : 0.0;
        for (std::uint32_t c = 0; c < channels; ++c) {
            const double baseHz = 200.0 + 120.0 * static_cast<double>(c);
            double sample = kBaseAmp * std::sin(2.0 * kPi * baseHz * t);
            if (c == kFcIndex && inDialogue) {
                const double envelope =
                    0.5 + 0.5 * std::sin(2.0 * kPi * 3.0 * t);
                sample += kDialogueAmp * envelope *
                          std::sin(2.0 * kPi * 900.0 * t);
            }
            if (c == kLfeIndex && inLfe) {
                sample += kLfeAmp * std::sin(2.0 * kPi * 45.0 * t);
            }
            sample += burst;
            program[static_cast<std::size_t>(n) * channels + c] =
                static_cast<float>(sample);
        }
    }
    return program;
}

bool WriteRawF32(const std::filesystem::path& path,
                 const std::vector<float>& interleaved) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(interleaved.data()),
              static_cast<std::streamsize>(interleaved.size() *
                                           sizeof(float)));
    return out.good();
}

// Renders `program` (programChannels interleaved) with the given mix/device
// layouts on the null backend and appends every rendered device sample,
// interleaved, to `out`. Returns the device channel count via outChannels, or
// false on any engine failure.
bool RenderCapture(std::uint32_t mixLayout, std::uint32_t deviceLayout,
                   const std::vector<float>& program,
                   std::uint32_t programChannels, std::vector<float>* out,
                   std::uint32_t* outChannels, const char* what) {
    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    config.blockFrames = kBlockFrames;
    config.sampleRate = kSampleRate;
    config.maxVoices = 4;
    config.mixChannelLayout = mixLayout;
    config.deviceChannelLayout = deviceLayout;
    if (Vn_Init(&config) != VN_OK) {
        std::fprintf(stderr, "error: Vn_Init failed for %s\n", what);
        return false;
    }
    harken::Engine* engine = harken::detail::ActiveEngine();

    VnPCMDesc desc{};
    desc.structSize = sizeof(VnPCMDesc);
    desc.sampleRate = kSampleRate;
    desc.channels = programChannels;
    desc.layout = VN_PCM_LAYOUT_INTERLEAVED;
    desc.policy = VN_PCM_POLICY_COPY;
    desc.frames = program.size() / programChannels;
    desc.samplesInterleaved = program.data();
    const VnObjectId sound{0x76656e7573686f73, 1};
    if (Vn_RegisterPCM(sound, &desc) != VN_OK) {
        std::fprintf(stderr, "error: Vn_RegisterPCM failed for %s\n", what);
        Vn_Term();
        return false;
    }

    VnPlayDesc play{};
    play.structSize = sizeof(VnPlayDesc);
    play.soundId = sound;
    play.busId = VN_BUS_MASTER;
    play.loopCount = 1; // single pass; the tail renders silence on both runs
    play.volumeDb = 0.0f;
    play.pitchRatio = 0.0f;
    VnVoiceHandle handle = 0;
    if (harken::detail::PlayVoiceRaw(&play, &handle) != VN_OK) {
        std::fprintf(stderr, "error: PlayVoiceRaw failed for %s\n", what);
        Vn_Term();
        return false;
    }
    (void)handle; // the drill drives blocks by time, not by the voice handle

    const std::uint64_t frames = program.size() / programChannels;
    const std::uint32_t blockFrames = engine->nullBackend.BlockFrames();
    const std::uint32_t channels = engine->nullBackend.Channels();
    *outChannels = channels;
    // Render enough whole blocks to cover the program; both runs use the same
    // frame count and block size, so their captures are frame-aligned.
    const std::uint64_t blocks =
        (frames + blockFrames - 1) / blockFrames;
    out->clear();
    out->reserve(static_cast<std::size_t>(blocks) * blockFrames * channels);
    for (std::uint64_t b = 0; b < blocks; ++b) {
        if (Vn_Update() != VN_OK) {
            std::fprintf(stderr, "error: Vn_Update failed for %s\n", what);
            Vn_Term();
            return false;
        }
        engine->nullBackend.Pump(1);
        const float* block = engine->nullBackend.LastBlock();
        out->insert(out->end(), block,
                    block + static_cast<std::size_t>(blockFrames) * channels);
    }
    Vn_Term();
    return true;
}

std::string JsonEscape(const std::filesystem::path& path) {
    const std::string s = std::filesystem::absolute(path).generic_string();
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

bool ParseSecondsArg(const char* text, double* out) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !(value >= 5.0) ||
        value > kMaxDrillSeconds) {
        return false;
    }
    *out = value;
    return true;
}

} // namespace

void PrintDownmixNullDrillUsage() {
    std::fprintf(
        stderr,
        "       harken_host downmix-null-drill [--seconds N] [--out <dir>]\n"
        "\n"
        "downmix-null-drill renders a deterministic 5.1 and 7.1 program\n"
        "through the master output fold on the null backend and writes, per\n"
        "layout, the pre-fold mix (source_<layout>.f32) and the folded stereo\n"
        "output (engine_downmix_<layout>.f32) as raw interleaved float32,\n"
        "plus a manifest.json. --seconds defaults to 6 and accepts [5, 60];\n"
        "--out defaults to .temp/nulldrill. Judge with\n"
        "  python HarkenEngine/Test/E2E/downmix_null_reference.py [<dir>]\n");
}

int RunDownmixNullDrill(int argc, char** argv) {
    double seconds = 6.0;
    std::filesystem::path outDir = ".temp/nulldrill";
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            if (!ParseSecondsArg(argv[++i], &seconds)) {
                std::fprintf(stderr,
                             "error: --seconds must be in [5, %.0f]\n",
                             kMaxDrillSeconds);
                return 1;
            }
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outDir = argv[++i];
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", argv[i]);
            PrintDownmixNullDrillUsage();
            return 1;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot create %s: %s\n",
                     outDir.string().c_str(), ec.message().c_str());
        return 1;
    }

    const std::uint64_t frames =
        static_cast<std::uint64_t>(seconds * kSampleRate);
    const DrillSpec drills[] = {
        {"5_1_to_stereo", VN_CHANNEL_LAYOUT_5_1, 6, "5.1",
         "source_5_1.f32", "engine_downmix_5_1.f32"},
        {"7_1_to_stereo", VN_CHANNEL_LAYOUT_7_1, 8, "7.1",
         "source_7_1.f32", "engine_downmix_7_1.f32"},
    };

    std::string manifestDrills;
    for (const DrillSpec& drill : drills) {
        const std::vector<float> program =
            GenerateProgram(drill.channels, frames);

        // Render A: device == mix (identity, no fold) -> the pre-fold mix.
        std::vector<float> source;
        std::uint32_t sourceChannels = 0;
        if (!RenderCapture(drill.mixLayout, drill.mixLayout, program,
                           drill.channels, &source, &sourceChannels,
                           drill.name)) {
            return 2;
        }
        // Render B: stereo device -> the master output fold.
        std::vector<float> folded;
        std::uint32_t foldedChannels = 0;
        if (!RenderCapture(drill.mixLayout, VN_CHANNEL_LAYOUT_STEREO, program,
                           drill.channels, &folded, &foldedChannels,
                           drill.name)) {
            return 2;
        }

        const std::filesystem::path sourcePath = outDir / drill.sourceFile;
        const std::filesystem::path enginePath = outDir / drill.engineFile;
        if (!WriteRawF32(sourcePath, source) ||
            !WriteRawF32(enginePath, folded)) {
            std::fprintf(stderr, "error: cannot write artifacts for %s\n",
                         drill.name);
            return 1;
        }

        const std::uint64_t sourceFrames =
            sourceChannels > 0 ? source.size() / sourceChannels : 0;
        const std::uint64_t foldedFrames =
            foldedChannels > 0 ? folded.size() / foldedChannels : 0;
        std::printf("drill %s: source %s (%u ch, %llu frames), engine %s "
                    "(%u ch, %llu frames)\n",
                    drill.name, drill.sourceFile, sourceChannels,
                    static_cast<unsigned long long>(sourceFrames),
                    drill.engineFile, foldedChannels,
                    static_cast<unsigned long long>(foldedFrames));

        if (!manifestDrills.empty()) {
            manifestDrills += ",\n";
        }
        manifestDrills +=
            "    { \"name\": \"" + std::string(drill.name) +
            "\", \"srcLayout\": \"" + drill.srcLayoutName +
            "\", \"dstLayout\": \"stereo\", \"source\": \"" +
            JsonEscape(sourcePath) + "\", \"sourceChannels\": " +
            std::to_string(sourceChannels) + ", \"engine\": \"" +
            JsonEscape(enginePath) + "\", \"deviceChannels\": " +
            std::to_string(foldedChannels) +
            ", \"frames\": " + std::to_string(sourceFrames) + " }";
    }

    const std::string manifest =
        "{\n  \"sampleRate\": " + std::to_string(kSampleRate) +
        ",\n  \"drills\": [\n" + manifestDrills + "\n  ]\n}\n";
    const std::filesystem::path manifestPath = outDir / "manifest.json";
    std::ofstream manifestOut(manifestPath, std::ios::binary | std::ios::trunc);
    if (!manifestOut) {
        std::fprintf(stderr, "error: cannot write %s\n",
                     manifestPath.string().c_str());
        return 1;
    }
    manifestOut.write(manifest.data(),
                      static_cast<std::streamsize>(manifest.size()));
    if (!manifestOut.good()) {
        std::fprintf(stderr, "error: writing %s failed\n",
                     manifestPath.string().c_str());
        return 1;
    }

    std::printf("downmix-null-drill wrote artifacts to %s\n"
                "judge with: python HarkenEngine/Test/E2E/downmix_null_reference.py %s\n",
                outDir.string().c_str(), outDir.string().c_str());
    return 0;
}

} // namespace harkenhost
