// Release metering benchmark: the bus output peak scan is compiled into
// ALL builds from M6,
// so this measures what that always-on scan costs per block at max buses —
// deliberately built WITHOUT the stage-tap opt-in, so a Release build of
// this executable times exactly the code a shipping Release build runs.
//
// Two measurements, reported honestly (the repository benchmark
// convention: numbers, not a timing-based correctness gate):
//   1. the isolated per-block scan — StageTapPeak across maxBuses buses of
//      mix-width planes at the benchmark block size (the section-11.7
//      adder in its worst case, every bus live);
//   2. the whole per-block render callback of a full scene (maxBuses live
//      buses, voices spread across them, a MeterProbe armed so the
//      ring append runs), whose budget the scan must live inside.
// The one hard exit: the isolated scan alone exceeding the entire
// per-block real-time budget would mean the section-11.7 gate cannot
// hold on any machine — everything short of that is a reported number
// for the acceptance judgment. It also verifies Release snapshots read
// honest (nonzero) busPeaks under signal, the section-11.7 observable.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Engine.h"
#include "Common/RawVoiceTestSupport.h"
#include "harken/core/SnapshotBuffer.h"
#include "harken/param/MeterProbe.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kMaxBuses = 64;
constexpr std::uint32_t kMixChannels = 8; // 7.1 — the widest mix layout
constexpr std::uint32_t kScanIterations = 2000;
constexpr std::uint32_t kSceneBlocks = 600;
constexpr std::uint32_t kSceneVoices = 32;

std::vector<float> MakeSine(std::size_t frames, double freqHz, float amp) {
    std::vector<float> s(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        s[n] = amp * static_cast<float>(
                         std::sin(2.0 * 3.14159265358979323846 * freqHz *
                                  static_cast<double>(n) /
                                  static_cast<double>(kRate)));
    }
    return s;
}

// Isolated worst case: every one of maxBuses buses scanned across the full
// mix width every block. Non-trivial signal content defeats any
// constant-folding; the accumulated checksum defeats dead-code elimination.
double IsolatedScanMicrosPerBlock() {
    std::vector<float> storage(
        static_cast<std::size_t>(kMaxBuses) * kMixChannels * kBlock);
    for (std::size_t i = 0; i < storage.size(); ++i) {
        storage[i] = std::sin(static_cast<float>(i) * 0.37f);
    }
    std::vector<const float*> planes(
        static_cast<std::size_t>(kMaxBuses) * kMixChannels);
    for (std::size_t p = 0; p < planes.size(); ++p) {
        planes[p] = storage.data() + p * kBlock;
    }
    volatile float sink = 0.0f;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint32_t iter = 0; iter < kScanIterations; ++iter) {
        float sum = 0.0f;
        for (std::uint32_t bus = 0; bus < kMaxBuses; ++bus) {
            sum += harken::StageTapPeak(
                planes.data() + static_cast<std::size_t>(bus) * kMixChannels,
                kMixChannels, 0, kBlock);
        }
        sink = sink + sum;
    }
    const auto t1 = std::chrono::steady_clock::now();
    (void)sink;
    const double totalUs =
        std::chrono::duration<double, std::micro>(t1 - t0).count();
    return totalUs / static_cast<double>(kScanIterations);
}

} // namespace

int main() {
    const double blockBudgetUs =
        1.0e6 * static_cast<double>(kBlock) / static_cast<double>(kRate);
    std::printf("Release metering benchmark: %u buses x %u channels x "
                "%u-frame blocks @ %u Hz (block budget %.0f us)\n",
                kMaxBuses, kMixChannels, kBlock, kRate, blockBudgetUs);

    const double scanUs = IsolatedScanMicrosPerBlock();
    std::printf("isolated bus output peak scan: %.2f us/block "
                "(%.2f%% of budget)\n",
                scanUs, 100.0 * scanUs / blockBudgetUs);

    // Full scene: maxBuses live buses, voices spread across them, one
    // MeterProbe armed so the per-block ring append is in the path.
    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    config.sampleRate = kRate;
    config.blockFrames = kBlock;
    config.maxVoices = 64;
    config.maxBuses = kMaxBuses;
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_7_1;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.arenaBytes = 1u << 22;
    // Every Vn_CreateBus is a structural change that publishes one
    // topology plan, and a plan slot frees only after a pump lets the
    // audio thread install its successor. This scene creates all
    // maxBuses - 1 buses before the first pump, so the pool must hold
    // one in-flight plan per create — the default (4) refuses the fifth
    // create with VN_BACKPRESSURE.
    config.topologyPlanPoolCapacity = 2 * kMaxBuses;
    if (Vn_Init(&config) != VN_OK) {
        std::printf("FAIL: engine init refused\n");
        return 2;
    }
    harken::Engine* engine = harken::detail::ActiveEngine();

    for (std::uint32_t b = 2; b <= kMaxBuses; ++b) {
        const VnResult created = Vn_CreateBus(b, VN_BUS_MASTER);
        if (created != VN_OK) {
            std::printf("FAIL: bus creation refused at %u (VnResult %d)\n",
                        b, static_cast<int>(created));
            Vn_Term();
            return 2;
        }
    }
    const VnResult paired = harken::InstallMeterProbe(
        *engine, 9001, 2, VN_BUS_METER_TAP_POST_EFFECTS, 10, 200);
    if (paired != VN_OK) {
        std::printf("FAIL: MeterProbe install refused (VnResult %d)\n",
                    static_cast<int>(paired));
        Vn_Term();
        return 2;
    }

    const std::size_t sourceFrames =
        static_cast<std::size_t>(kSceneBlocks) * kBlock + kRate;
    const std::vector<float> sine = MakeSine(sourceFrames, 220.0, 0.25f);
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(VnPCMDesc);
    pcm.sampleRate = kRate;
    pcm.channels = 1;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_COPY;
    pcm.frames = sine.size();
    pcm.samplesInterleaved = sine.data();
    Vn_RegisterPCM(harkentest::ObjectId(1), &pcm);
    for (std::uint32_t v = 0; v < kSceneVoices; ++v) {
        VnPlayDesc play{};
        play.structSize = sizeof(VnPlayDesc);
        play.soundId = harkentest::ObjectId(1);
        play.busId = 2 + (v % (kMaxBuses - 1)); // spread across every bus
        play.volumeDb = -12.0f;
        play.loopCount = 1;
        VnVoiceHandle handle = 0;
        (void)harkentest::PlayRaw(&play, &handle);
    }
    Vn_Update();

    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint32_t b = 0; b < kSceneBlocks; ++b) {
        engine->nullBackend.Pump(1);
        if ((b & 7u) == 7u) {
            Vn_Update(); // realistic drain cadence for the meter ring
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    Vn_Update();
    const double sceneWallUs =
        std::chrono::duration<double, std::micro>(t1 - t0).count();
    const double perBlockUs =
        sceneWallUs / static_cast<double>(kSceneBlocks);

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    bool peaksHonest = false;
    std::uint32_t audioCpuPeakUs = 0;
    if (Vn_CaptureSnapshot(&snapshot) == VN_OK) {
        audioCpuPeakUs = snapshot.audioCpuPeakUs;
        for (std::uint32_t i = 0; i < VN_SNAPSHOT_MAX_BUSES; ++i) {
            if (snapshot.busPeaks[i] > 0.0f) {
                peaksHonest = true;
                break;
            }
        }
    }
    Vn_Term();

    std::printf("full scene (%u voices, %u buses, meter armed): "
                "%.2f us/block wall, audioCpuPeak %u us "
                "(budget %.0f us)\n",
                kSceneVoices, kMaxBuses, perBlockUs, audioCpuPeakUs,
                blockBudgetUs);
    std::printf("snapshot busPeaks under signal: %s\n",
                peaksHonest ? "nonzero (honest metering live)" : "ALL ZERO");

    if (!peaksHonest) {
        std::printf("FAIL: this build's snapshot bus peaks read zero — the "
                    "section-11.7 scan is not live\n");
        return 1;
    }
    if (scanUs > blockBudgetUs) {
        std::printf("FAIL: the isolated peak scan alone exceeds the whole "
                    "per-block budget\n");
        return 1;
    }
    std::printf("scan inside the per-block budget.\n");
    return 0;
}
