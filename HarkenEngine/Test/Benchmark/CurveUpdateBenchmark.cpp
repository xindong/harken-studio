// Logic-tick curve-evaluation benchmark for the maximum configured voice and
// binding scales. The workload deliberately queries the eighth-point segment,
// forcing the full seven-segment scan and one fixed 32-iteration Bezier
// inversion per Eval. It reports timing only and has no performance threshold.
#include "harken/param/Curve.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr std::uint32_t kMaxConfiguredVoices = 1u << 14;
constexpr std::uint32_t kMaxConfiguredBindings = 1u << 16;
constexpr std::uint32_t kAttenuationCurvesPerVoice = 3;
constexpr std::uint32_t kDefaultWarmupTicks = 8;
constexpr std::uint32_t kDefaultMeasuredTicks = 64;

enum class Scenario {
    Voices,
    Bindings,
    Combined,
    All
};

struct Options {
    Scenario scenario = Scenario::Combined;
    std::uint32_t warmupTicks = kDefaultWarmupTicks;
    std::uint32_t measuredTicks = kDefaultMeasuredTicks;
};

volatile double g_curveUpdateSink = 0.0;

void PrintUsage() {
    std::printf(
        "Usage: harken_benchmarks --filter curve-update "
        "[--scenario voices|bindings|combined|all] "
        "[--warmup N] [--ticks N]\n");
}

bool ParseCount(const char* text, std::uint32_t* out) {
    if (text == nullptr || text[0] == '\0' || out == nullptr) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    *out = static_cast<std::uint32_t>(parsed);
    return true;
}

bool ParseOptions(int argc, char* argv[], Options* out) {
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scenario") == 0) {
            if (++i >= argc) {
                return false;
            }
            if (std::strcmp(argv[i], "voices") == 0) {
                out->scenario = Scenario::Voices;
            } else if (std::strcmp(argv[i], "bindings") == 0) {
                out->scenario = Scenario::Bindings;
            } else if (std::strcmp(argv[i], "combined") == 0) {
                out->scenario = Scenario::Combined;
            } else if (std::strcmp(argv[i], "all") == 0) {
                out->scenario = Scenario::All;
            } else {
                return false;
            }
        } else if (std::strcmp(argv[i], "--warmup") == 0) {
            if (++i >= argc || !ParseCount(argv[i], &out->warmupTicks)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--ticks") == 0) {
            if (++i >= argc || !ParseCount(argv[i], &out->measuredTicks)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--help") == 0) {
            PrintUsage();
            std::exit(0);
        } else {
            return false;
        }
    }
    return true;
}

harken::Curve MakeWorstCaseCurve(float yScale) {
    std::array<harken::CurvePoint, harken::kMaxCurvePoints> points{};
    std::array<harken::CurveHandlePair, harken::kMaxCurvePoints - 1> handles{};
    for (std::uint32_t i = 0; i < harken::kMaxCurvePoints; ++i) {
        points[i] = {static_cast<float>(i),
                     -static_cast<float>(i * i + i) * yScale};
    }
    for (std::uint32_t i = 0; i + 1 < harken::kMaxCurvePoints; ++i) {
        handles[i] = {points[i + 1].x, points[i].y, points[i].x,
                      points[i + 1].y};
    }

    harken::Curve curve;
    if (!curve.SetPoints(points.data(), handles.data(),
                         harken::kMaxCurvePoints)) {
        std::fprintf(stderr, "Failed to construct benchmark curve\n");
        std::exit(2);
    }
    return curve;
}

struct Workload {
    const char* name = nullptr;
    std::uint32_t voices = 0;
    std::uint32_t bindings = 0;
};

void RunWorkload(const Workload& workload, const Options& options) {
    const std::array<harken::Curve, kAttenuationCurvesPerVoice>
        attenuationCurves = {MakeWorstCaseCurve(1.0f),
                             MakeWorstCaseCurve(0.5f),
                             MakeWorstCaseCurve(0.25f)};
    const harken::Curve bindingCurve = MakeWorstCaseCurve(0.75f);
    const std::vector<harken::Curve> bindingCurves(
        workload.bindings, bindingCurve);
    const std::uint64_t evaluationsPerTick =
        static_cast<std::uint64_t>(workload.voices) *
            kAttenuationCurvesPerVoice +
        workload.bindings;

    double elapsedUsSum = 0.0;
    double elapsedUsPeak = 0.0;
    double sink = 0.0;
    const std::uint64_t totalTicks =
        static_cast<std::uint64_t>(options.warmupTicks) +
        options.measuredTicks;

    for (std::uint64_t tick = 0; tick < totalTicks; ++tick) {
        const float x = 6.125f +
                        static_cast<float>(tick % 1024) * (0.75f / 1024.0f);
        const auto begin = std::chrono::steady_clock::now();
        for (std::uint32_t voice = 0; voice < workload.voices; ++voice) {
            sink += attenuationCurves[0].Eval(x);
            sink += attenuationCurves[1].Eval(x);
            sink += attenuationCurves[2].Eval(x);
        }
        for (const harken::Curve& curve : bindingCurves) {
            sink += curve.Eval(x);
        }
        const auto end = std::chrono::steady_clock::now();

        if (tick >= options.warmupTicks) {
            const double elapsedUs =
                std::chrono::duration<double, std::micro>(end - begin).count();
            elapsedUsSum += elapsedUs;
            elapsedUsPeak = std::max(elapsedUsPeak, elapsedUs);
        }
    }
    g_curveUpdateSink = sink;

    const double meanUs =
        elapsedUsSum / static_cast<double>(options.measuredTicks);
    const double meanNsPerEval =
        evaluationsPerTick == 0
            ? 0.0
            : meanUs * 1000.0 / static_cast<double>(evaluationsPerTick);
    std::printf("%-10s %10u %10u %12llu %14.3f %14.3f %14.3f\n",
                workload.name, workload.voices, workload.bindings,
                static_cast<unsigned long long>(evaluationsPerTick), meanUs,
                elapsedUsPeak, meanNsPerEval);
}

} // namespace

int RunCurveUpdateBenchmark(int argc, char* argv[]) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintUsage();
        return 2;
    }

    std::printf(
        "Bezier curve update workload: 8 points, last-segment query, "
        "7 segment scans + 32 bisection iterations per Eval\n");
    std::printf("Warmup ticks: %u; measured ticks: %u\n", options.warmupTicks,
                options.measuredTicks);
    std::printf("%-10s %10s %10s %12s %14s %14s %14s\n", "scenario",
                "voices", "bindings", "evals/tick", "mean_us/tick",
                "peak_us/tick", "mean_ns/eval");

    const Workload voices{"voices", kMaxConfiguredVoices, 0};
    const Workload bindings{"bindings", 0, kMaxConfiguredBindings};
    const Workload combined{"combined", kMaxConfiguredVoices,
                            kMaxConfiguredBindings};
    switch (options.scenario) {
    case Scenario::Voices:
        RunWorkload(voices, options);
        break;
    case Scenario::Bindings:
        RunWorkload(bindings, options);
        break;
    case Scenario::Combined:
        RunWorkload(combined, options);
        break;
    case Scenario::All:
        RunWorkload(voices, options);
        RunWorkload(bindings, options);
        RunWorkload(combined, options);
        break;
    }
    return 0;
}
