// Real WASAPI callback scheduling measurement. The host owns the device
// callback and drives the engine's null backend from it, preserving the
// production fixed-block staging behavior without changing engine code.
#include "SchedulingMeasure.h"

#include "DemoOptions.h"
#include "SchedulingMeasureOptions.h"

#include <harken/Harken.h>

#include "Engine.h"
#include "RenderSlicer.h"
#include "HarkenApiInternal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

#include "miniaudio.h"
#endif

namespace harkenhost {

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kEngineBlockFrames = 256;
constexpr std::uint32_t kDeviceChannels = 2;
constexpr std::uint32_t kRequestedPeriodFrames = kSampleRate / 50;
constexpr std::uint32_t kSnapshotPeriodMs = 100;
constexpr VnBusId kSendBus{0x76656e7573686f73, 7};

struct SchedulingOptions {
    std::uint32_t workers = 0;
    SchedulingLoad load = SchedulingLoad::Light;
    double seconds = 30.0;
    std::string csvPath;
    std::uint32_t cpuThreads = 0;
    std::uint32_t cpuDuty = 0;
};

const char* LoadName(SchedulingLoad load) {
    switch (load) {
    case SchedulingLoad::Light:
        return "light";
    case SchedulingLoad::Medium:
        return "medium";
    case SchedulingLoad::Heavy:
        return "heavy";
    }
    return "unknown";
}

bool ParseOptions(int argc, char** argv, SchedulingOptions* out) {
    if (argc < 2 || std::strcmp(argv[1], "sched-measure") != 0) {
        return false;
    }
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--workers" && i + 1 < argc) {
            if (!ParseSchedulingWorkerCount(argv[++i], &out->workers)) {
                std::fprintf(stderr,
                             "error: --workers must be one of 0,1,2,4,8\n");
                return false;
            }
        } else if (arg == "--load" && i + 1 < argc) {
            if (!ParseSchedulingLoad(argv[++i], &out->load)) {
                std::fprintf(stderr,
                             "error: --load must be light, medium, or heavy\n");
                return false;
            }
        } else if (arg == "--seconds" && i + 1 < argc) {
            if (!ParseSeconds(argv[++i], &out->seconds)) {
                std::fprintf(stderr,
                             "error: --seconds must be a finite number in "
                             "(0, %.0f]\n",
                             kMaxDemoSeconds);
                return false;
            }
        } else if (arg == "--csv" && i + 1 < argc) {
            out->csvPath = argv[++i];
            if (out->csvPath.empty()) {
                std::fprintf(stderr, "error: --csv path must not be empty\n");
                return false;
            }
        } else if (arg == "--cpu-threads" && i + 1 < argc) {
            if (!ParseSchedulingCpuThreads(argv[++i], &out->cpuThreads)) {
                std::fprintf(stderr,
                             "error: --cpu-threads must be an integer in "
                             "[0,256]\n");
                return false;
            }
        } else if (arg == "--cpu-duty" && i + 1 < argc) {
            if (!ParseSchedulingCpuDuty(argv[++i], &out->cpuDuty)) {
                std::fprintf(stderr,
                             "error: --cpu-duty must be one of 0,50,90\n");
                return false;
            }
        } else {
            std::fprintf(stderr, "error: unknown or incomplete argument '%s'\n",
                         arg.c_str());
            return false;
        }
    }
    if (out->csvPath.empty()) {
        std::fprintf(stderr, "error: --csv is required\n");
        return false;
    }
    if ((out->cpuDuty == 0) != (out->cpuThreads == 0)) {
        std::fprintf(stderr,
                     "error: baseline requires --cpu-duty 0 with "
                     "--cpu-threads 0; nonzero duty requires nonzero threads\n");
        return false;
    }
    return true;
}

#if defined(_WIN32)

std::uint64_t QpcTicks() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t QpcFrequency() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

double TicksToMicros(std::uint64_t ticks, std::uint64_t frequency) {
    return static_cast<double>(ticks) * 1000000.0 /
           static_cast<double>(frequency);
}

struct CallbackRecord {
    std::uint64_t sequence = 0;
    std::uint64_t arrivalQpc = 0;
    std::uint64_t renderTicks = 0;
    std::uint32_t frames = 0;
};

class CallbackRecordQueue {
public:
    CallbackRecordQueue() : m_records(kCapacity) {}

    void Push(const CallbackRecord& record) noexcept {
        const std::uint64_t write = m_write.load(std::memory_order_relaxed);
        const std::uint64_t read = m_read.load(std::memory_order_acquire);
        if (write - read >= kCapacity) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_records[static_cast<std::size_t>(write & (kCapacity - 1))] =
            record;
        m_write.store(write + 1, std::memory_order_release);
        std::uint64_t expected = 0;
        m_firstArrival.compare_exchange_strong(
            expected, record.arrivalQpc, std::memory_order_release,
            std::memory_order_relaxed);
    }

    void Drain(std::vector<CallbackRecord>* out) {
        std::uint64_t read = m_read.load(std::memory_order_relaxed);
        const std::uint64_t write = m_write.load(std::memory_order_acquire);
        while (read != write) {
            out->push_back(
                m_records[static_cast<std::size_t>(read & (kCapacity - 1))]);
            ++read;
        }
        m_read.store(read, std::memory_order_release);
    }

    std::uint64_t FirstArrival() const {
        return m_firstArrival.load(std::memory_order_acquire);
    }

    std::uint64_t Dropped() const {
        return m_dropped.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::uint64_t kCapacity = 262144;
    static_assert((kCapacity & (kCapacity - 1)) == 0,
                  "queue capacity must be a power of two");

    std::vector<CallbackRecord> m_records;
    std::atomic<std::uint64_t> m_write{0};
    std::atomic<std::uint64_t> m_read{0};
    std::atomic<std::uint64_t> m_firstArrival{0};
    std::atomic<std::uint64_t> m_dropped{0};
};

struct SnapshotRecord {
    std::uint64_t sequence = 0;
    std::uint64_t qpc = 0;
    std::uint64_t blocksRendered = 0;
    std::uint32_t audioCpuLastUs = 0;
    std::uint32_t audioCpuPeakUs = 0;
    std::uint32_t renderWorkerBusyPeakUs = 0;
};

class EngineLifetime {
public:
    ~EngineLifetime() {
        if (m_active) {
            Vn_Term();
        }
    }

    bool Init(const SchedulingOptions& options, std::uint32_t voices) {
        VnConfig config{};
        config.structSize = sizeof(VnConfig);
        config.deviceMode = VN_DEVICE_NULL;
        config.maxVoices = voices;
        config.blockFrames = kEngineBlockFrames;
        config.sampleRate = kSampleRate;
        config.fxSlotsPerVoice = 2;
        config.delayEffectPoolCapacity = voices;
        config.toneEffectPoolCapacity = voices;
        config.voiceSendSlots = 1;
        config.mixChannelLayout = VN_CHANNEL_LAYOUT_5_1;
        config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
        config.renderWorkerCount = options.workers;
        config.arenaBytes = 1u << 23;
        const VnResult result = Vn_Init(&config);
        if (result != VN_OK) {
            std::fprintf(stderr, "error: Vn_Init failed (VnResult %d)\n",
                         static_cast<int>(result));
            return false;
        }
        m_active = true;
        return true;
    }

private:
    bool m_active = false;
};

class CpuCompetition {
public:
    ~CpuCompetition() { Stop(); }

    bool Start(std::uint32_t threadCount, std::uint32_t duty) {
        m_stop.store(false, std::memory_order_relaxed);
        try {
            for (std::uint32_t i = 0; i < threadCount; ++i) {
                m_threads.emplace_back([this, duty] { RunThread(duty); });
            }
        } catch (...) {
            Stop();
            return false;
        }
        return true;
    }

    void Stop() {
        m_stop.store(true, std::memory_order_release);
        for (std::thread& thread : m_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        m_threads.clear();
    }

private:
    void RunThread(std::uint32_t duty) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        constexpr auto cycle = std::chrono::milliseconds(10);
        const auto spinFor = std::chrono::microseconds(duty * 100);
        while (!m_stop.load(std::memory_order_acquire)) {
            const auto cycleStart = std::chrono::steady_clock::now();
            const auto spinEnd = cycleStart + spinFor;
            while (std::chrono::steady_clock::now() < spinEnd &&
                   !m_stop.load(std::memory_order_relaxed)) {
                YieldProcessor();
            }
            std::this_thread::sleep_until(cycleStart + cycle);
        }
    }

    std::atomic<bool> m_stop{false};
    std::vector<std::thread> m_threads;
};

class MeasurementDevice {
public:
    ~MeasurementDevice() { Stop(); }

    bool Start(harken::Engine* engine, CallbackRecordQueue* records) {
        m_engine = engine;
        m_records = records;
        m_blockFrames = engine->nullBackend.BlockFrames();
        m_channels = engine->nullBackend.Channels();

        const ma_backend backend = ma_backend_wasapi;
        ma_context_config contextConfig = ma_context_config_init();
        if (ma_context_init(&backend, 1, &contextConfig, &m_context) !=
            MA_SUCCESS) {
            std::fprintf(stderr, "error: cannot initialize WASAPI context\n");
            return false;
        }
        m_contextReady = true;

        ma_device_config deviceConfig =
            ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.format = ma_format_f32;
        deviceConfig.playback.channels = kDeviceChannels;
        deviceConfig.playback.shareMode = ma_share_mode_shared;
        deviceConfig.sampleRate = kSampleRate;
        deviceConfig.periodSizeInFrames = kRequestedPeriodFrames;
        deviceConfig.periods = 2;
        deviceConfig.dataCallback = &MeasurementDevice::DataCallback;
        deviceConfig.pUserData = this;
        if (ma_device_init(&m_context, &deviceConfig, &m_device) !=
            MA_SUCCESS) {
            std::fprintf(stderr,
                         "error: cannot open default WASAPI output device\n");
            Stop();
            return false;
        }
        m_deviceReady = true;
        if (ma_device_start(&m_device) != MA_SUCCESS) {
            std::fprintf(stderr, "error: cannot start WASAPI output device\n");
            Stop();
            return false;
        }
        m_started = true;
        return true;
    }

    void Stop() {
        if (m_started) {
            ma_device_stop(&m_device);
            m_started = false;
        }
        if (m_deviceReady) {
            ma_device_uninit(&m_device);
            m_deviceReady = false;
        }
        if (m_contextReady) {
            ma_context_uninit(&m_context);
            m_contextReady = false;
        }
    }

private:
    static void DataCallback(ma_device* device, void* output,
                             const void* /*input*/, ma_uint32 frameCount) {
        static_cast<MeasurementDevice*>(device->pUserData)
            ->Render(output, static_cast<std::uint32_t>(frameCount));
    }

    void Render(void* output, std::uint32_t frameCount) noexcept {
        const std::uint64_t arrival = QpcTicks();
        float* out = static_cast<float*>(output);
        harken::PumpStagedBlocks(
            m_cursor, frameCount, m_blockFrames,
            [&] { m_engine->nullBackend.Pump(1); },
            [&](std::uint32_t dstFrames, std::uint32_t srcFrames,
                std::uint32_t frames) {
                std::memcpy(
                    out + static_cast<std::size_t>(dstFrames) * m_channels,
                    m_engine->nullBackend.LastBlock() +
                        static_cast<std::size_t>(srcFrames) * m_channels,
                    static_cast<std::size_t>(frames) * m_channels *
                        sizeof(float));
            });
        const std::uint64_t finished = QpcTicks();

        CallbackRecord record;
        record.sequence =
            m_sequence.fetch_add(1, std::memory_order_relaxed);
        record.arrivalQpc = arrival;
        record.renderTicks = finished - arrival;
        record.frames = frameCount;
        m_records->Push(record);
    }

    ma_context m_context{};
    ma_device m_device{};
    harken::Engine* m_engine = nullptr;
    CallbackRecordQueue* m_records = nullptr;
    harken::BlockStagingCursor m_cursor;
    std::atomic<std::uint64_t> m_sequence{0};
    std::uint32_t m_blockFrames = 0;
    std::uint32_t m_channels = 0;
    bool m_contextReady = false;
    bool m_deviceReady = false;
    bool m_started = false;
};

struct LoadSpec {
    std::uint32_t voices;
    std::uint32_t effectStride;
};

LoadSpec SpecForLoad(SchedulingLoad load) {
    switch (load) {
    case SchedulingLoad::Light:
        return {12, 4};
    case SchedulingLoad::Medium:
        return {32, 2};
    case SchedulingLoad::Heavy:
        return {64, 1};
    }
    return {0, 1};
}

const VnEffectDesc* AsBase(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

bool Check(VnResult result, const char* operation) {
    if (result == VN_OK) {
        return true;
    }
    std::fprintf(stderr, "error: %s failed (VnResult %d)\n", operation,
                 static_cast<int>(result));
    return false;
}

bool BuildScene(const LoadSpec& spec) {
    if (!Check(Vn_CreateBus(kSendBus, VN_BUS_MASTER), "Vn_CreateBus")) {
        return false;
    }

    constexpr std::uint32_t sourceCount = 8;
    std::vector<std::vector<float>> sources(sourceCount);
    for (std::uint32_t source = 0; source < sourceCount; ++source) {
        std::vector<float>& samples = sources[source];
        samples.resize(kSampleRate);
        const double frequency = 110.0 + 37.0 * source;
        for (std::size_t frame = 0; frame < samples.size(); ++frame) {
            samples[frame] = 0.05f * static_cast<float>(
                std::sin(2.0 * 3.14159265358979323846 * frequency *
                         static_cast<double>(frame) / kSampleRate));
        }

        VnPCMDesc desc{};
        desc.structSize = sizeof(VnPCMDesc);
        desc.sampleRate = kSampleRate;
        desc.channels = 1;
        desc.layout = VN_PCM_LAYOUT_INTERLEAVED;
        desc.policy = VN_PCM_POLICY_COPY;
        desc.frames = samples.size();
        desc.samplesInterleaved = samples.data();
        if (!Check(Vn_RegisterPCM(VnSoundId{0x458f0e9208bd437cull, source + 1}, &desc),
                   "Vn_RegisterPCM")) {
            return false;
        }
    }

    VnFeedbackDelayDesc delay{};
    delay.structSize = sizeof(VnFeedbackDelayDesc);
    delay.delayMs = 23.0f;
    delay.feedback = 0.42f;
    delay.wet = 0.45f;
    delay.dry = 1.0f;

    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = 3.0f;
    tone.lowShelfFreqHz = 180.0f;
    tone.peakGainDb = -4.0f;
    tone.peakFreqHz = 1700.0f;
    tone.peakQ = 1.4f;
    tone.highShelfGainDb = 2.0f;
    tone.highShelfFreqHz = 7200.0f;

    for (std::uint32_t voice = 0; voice < spec.voices; ++voice) {
        VnPlayDesc play{};
        play.structSize = sizeof(VnPlayDesc);
        play.soundId = VnSoundId{0x458f0e9208bd437cull, 1 + voice % sourceCount};
        play.busId = VN_BUS_MASTER;
        play.volumeDb = -30.0f;
        play.pitchRatio = 0.75f + 0.01f * static_cast<float>(voice % 20);
        play.loopCount = 0;
        play.spatialized = 1;
        play.positionX = -8.0f + 0.25f * static_cast<float>(voice % 64);
        play.positionZ = -4.0f - static_cast<float>(voice % 5);

        VnVoiceHandle handle = 0;
        if (!Check(harken::detail::PlayVoiceRaw(&play, &handle),
                   "PlayVoiceRaw")) {
            return false;
        }
        if ((voice % spec.effectStride) == 0) {
            if (!Check(Vn_SetVoiceEffect(handle, 0,
                                         VN_EFFECT_FEEDBACK_DELAY,
                                         AsBase(&delay)),
                       "Vn_SetVoiceEffect(delay)") ||
                !Check(Vn_SetVoiceEffect(handle, 1,
                                         VN_EFFECT_THREE_BAND_TONE,
                                         AsBase(&tone)),
                       "Vn_SetVoiceEffect(tone)")) {
                return false;
            }
        }
        if ((voice % 4) == 0 &&
            !Check(Vn_SetVoiceSend(handle, 0, kSendBus, -9.0f),
                   "Vn_SetVoiceSend")) {
            return false;
        }
    }
    return Check(Vn_Update(), "Vn_Update(scene setup)");
}

bool CaptureSnapshot(std::uint64_t sequence,
                     std::vector<SnapshotRecord>* snapshots) {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    if (Vn_CaptureSnapshot(&snapshot) != VN_OK) {
        return false;
    }
    SnapshotRecord record;
    record.sequence = sequence;
    record.qpc = QpcTicks();
    record.blocksRendered = snapshot.blockIndex;
    record.audioCpuLastUs = snapshot.audioCpuLastUs;
    record.audioCpuPeakUs = snapshot.audioCpuPeakUs;
    record.renderWorkerBusyPeakUs = snapshot.renderWorkerBusyPeakUs;
    snapshots->push_back(record);
    return true;
}

double Percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double rank = std::ceil(fraction * values.size());
    const std::size_t index =
        static_cast<std::size_t>(rank > 1.0 ? rank - 1.0 : 0.0);
    return values[index < values.size() ? index : values.size() - 1];
}

double CallbackBudgetMicros(std::uint32_t callbackFrames) {
    return static_cast<double>(callbackFrames) * 1000000.0 / kSampleRate;
}

void WriteCallbackCsv(std::ofstream& csv, const CallbackRecord& record,
                      std::uint64_t previousArrival,
                      std::uint64_t qpcFrequency,
                      const SchedulingOptions& options) {
    const double intervalUs =
        previousArrival == 0
            ? 0.0
            : TicksToMicros(record.arrivalQpc - previousArrival,
                            qpcFrequency);
    const double renderUs =
        TicksToMicros(record.renderTicks, qpcFrequency);
    const double budgetUs = CallbackBudgetMicros(record.frames);
    // Deadline and period utilization use the callback period; block_frames
    // reports the engine's per-block work quantum.
    csv << "callback," << record.sequence << ',' << record.arrivalQpc << ','
        << qpcFrequency << ',';
    if (previousArrival != 0) {
        csv << intervalUs;
    }
    csv << ',' << renderUs << ',' << record.frames << ','
        << kEngineBlockFrames << ',' << budgetUs << ','
        << (renderUs > budgetUs ? 1 : 0)
        << ",,,,," << options.workers << ',' << LoadName(options.load) << ','
        << options.cpuThreads << ',' << options.cpuDuty << '\n';
}

void WriteSnapshotCsv(std::ofstream& csv, const SnapshotRecord& record,
                      std::uint64_t qpcFrequency,
                      const SchedulingOptions& options) {
    csv << "snapshot," << record.sequence << ',' << record.qpc << ','
        << qpcFrequency << ",,,,,,,"
        << record.blocksRendered << ',' << record.audioCpuLastUs << ','
        << record.audioCpuPeakUs << ','
        << record.renderWorkerBusyPeakUs << ',' << options.workers << ','
        << LoadName(options.load) << ',' << options.cpuThreads << ','
        << options.cpuDuty << '\n';
}

bool WriteCsv(std::ofstream& csv,
              const std::vector<CallbackRecord>& callbacks,
              const std::vector<SnapshotRecord>& snapshots,
              std::uint64_t qpcFrequency,
              const SchedulingOptions& options) {
    csv << "record_type,sequence,qpc_ticks,qpc_frequency,"
           "arrival_interval_us,render_us,callback_frames,block_frames,"
           "budget_us,deadline_miss,blocks_rendered,audio_cpu_last_us,"
           "audio_cpu_peak_us,render_worker_busy_peak_us,worker_count,"
           "load,cpu_threads,cpu_duty\n";

    std::size_t callbackIndex = 0;
    std::size_t snapshotIndex = 0;
    std::uint64_t previousArrival = 0;
    while (callbackIndex < callbacks.size() ||
           snapshotIndex < snapshots.size()) {
        const bool takeCallback =
            snapshotIndex == snapshots.size() ||
            (callbackIndex < callbacks.size() &&
             callbacks[callbackIndex].arrivalQpc <=
                 snapshots[snapshotIndex].qpc);
        if (takeCallback) {
            const CallbackRecord& record = callbacks[callbackIndex++];
            WriteCallbackCsv(csv, record, previousArrival, qpcFrequency,
                             options);
            previousArrival = record.arrivalQpc;
        } else {
            WriteSnapshotCsv(csv, snapshots[snapshotIndex++], qpcFrequency,
                             options);
        }
    }
    csv.flush();
    return csv.good();
}

void PrintSummary(const std::vector<CallbackRecord>& callbacks,
                  const std::vector<SnapshotRecord>& snapshots,
                  std::uint64_t qpcFrequency,
                  std::uint64_t dropped) {
    std::vector<double> renderUs;
    std::vector<double> intervalUs;
    std::vector<double> periodUtilization;
    renderUs.reserve(callbacks.size());
    periodUtilization.reserve(callbacks.size());
    if (!callbacks.empty()) {
        intervalUs.reserve(callbacks.size() - 1);
    }

    std::uint64_t misses = 0;
    double renderPeakUs = 0.0;
    double intervalPeakUs = 0.0;
    double budgetMinUs = callbacks.empty()
                             ? 0.0
                             : (std::numeric_limits<double>::max)();
    double budgetMaxUs = 0.0;
    double utilizationPeak = 0.0;
    for (std::size_t i = 0; i < callbacks.size(); ++i) {
        const CallbackRecord& callback = callbacks[i];
        const double render =
            TicksToMicros(callback.renderTicks, qpcFrequency);
        const double budget = CallbackBudgetMicros(callback.frames);
        const double utilization = budget > 0.0 ? render / budget : 0.0;
        renderUs.push_back(render);
        periodUtilization.push_back(utilization);
        renderPeakUs = (std::max)(renderPeakUs, render);
        budgetMinUs = (std::min)(budgetMinUs, budget);
        budgetMaxUs = (std::max)(budgetMaxUs, budget);
        utilizationPeak = (std::max)(utilizationPeak, utilization);
        if (render > budget) {
            ++misses;
        }
        if (i != 0) {
            const double interval = TicksToMicros(
                callback.arrivalQpc - callbacks[i - 1].arrivalQpc,
                qpcFrequency);
            intervalUs.push_back(interval);
            intervalPeakUs = (std::max)(intervalPeakUs, interval);
        }
    }

    std::uint32_t audioCpuPeakUs = 0;
    std::uint32_t workerBusyPeakUs = 0;
    for (const SnapshotRecord& snapshot : snapshots) {
        audioCpuPeakUs =
            (std::max)(audioCpuPeakUs, snapshot.audioCpuPeakUs);
        workerBusyPeakUs =
            (std::max)(workerBusyPeakUs,
                       snapshot.renderWorkerBusyPeakUs);
    }

    std::printf("scheduling measurement summary:\n");
    std::printf("  callbacks=%llu snapshots=%llu queueDropped=%llu\n",
                static_cast<unsigned long long>(callbacks.size()),
                static_cast<unsigned long long>(snapshots.size()),
                static_cast<unsigned long long>(dropped));
    std::printf("  render_us peak=%.3f p99=%.3f p999=%.3f\n",
                renderPeakUs, Percentile(renderUs, 0.99),
                Percentile(renderUs, 0.999));
    std::printf("  deadline callback_budget_us min=%.3f max=%.3f "
                "(callback frames @ %u Hz)\n",
                budgetMinUs, budgetMaxUs, kSampleRate);
    std::printf("  period_utilization peak=%.6f p99=%.6f\n",
                utilizationPeak, Percentile(periodUtilization, 0.99));
    std::printf("  arrival_interval_us peak=%.3f p99=%.3f p999=%.3f\n",
                intervalPeakUs, Percentile(intervalUs, 0.99),
                Percentile(intervalUs, 0.999));
    std::printf("  deadline misses=%llu hits=%llu\n",
                static_cast<unsigned long long>(misses),
                static_cast<unsigned long long>(callbacks.size() - misses));
    std::printf("  snapshot audioCpuPeakUs=%u "
                "renderWorkerBusyPeakUs=%u\n",
                audioCpuPeakUs, workerBusyPeakUs);
}

int RunWindowsMeasurement(const SchedulingOptions& options) {
    std::ofstream csv(options.csvPath,
                      std::ios::out | std::ios::trunc);
    if (!csv.is_open()) {
        std::fprintf(stderr, "error: cannot open CSV output '%s'\n",
                     options.csvPath.c_str());
        return 1;
    }

    const LoadSpec spec = SpecForLoad(options.load);
    EngineLifetime engineLifetime;
    if (!engineLifetime.Init(options, spec.voices)) {
        return 3;
    }
    if (!BuildScene(spec)) {
        return 3;
    }

    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        std::fprintf(stderr, "error: engine instance is unavailable\n");
        return 3;
    }

    CallbackRecordQueue recordQueue;
    std::vector<CallbackRecord> callbacks;
    callbacks.reserve(
        static_cast<std::size_t>(options.seconds * 100.0) + 1024);
    std::vector<SnapshotRecord> snapshots;
    snapshots.reserve(
        static_cast<std::size_t>(options.seconds * 10.0) + 16);

    CpuCompetition competition;
    if (!competition.Start(options.cpuThreads, options.cpuDuty)) {
        std::fprintf(stderr, "error: cannot start CPU competition threads\n");
        return 3;
    }

    MeasurementDevice device;
    if (!device.Start(engine, &recordQueue)) {
        return 3;
    }

    const std::uint64_t frequency = QpcFrequency();
    if (frequency == 0) {
        std::fprintf(stderr, "error: QueryPerformanceFrequency failed\n");
        return 3;
    }
    const auto startupDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (recordQueue.FirstArrival() == 0 &&
           std::chrono::steady_clock::now() < startupDeadline) {
        recordQueue.Drain(&callbacks);
        (void)Vn_Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const std::uint64_t firstArrival = recordQueue.FirstArrival();
    if (firstArrival == 0) {
        std::fprintf(stderr,
                     "error: WASAPI produced no callback within five seconds\n");
        return 3;
    }

    const std::uint64_t durationTicks = static_cast<std::uint64_t>(
        options.seconds * static_cast<double>(frequency));
    const std::uint64_t endQpc = firstArrival + durationTicks;
    std::uint64_t nextSnapshotQpc = firstArrival;
    std::uint64_t snapshotSequence = 0;

    std::printf("sched-measure: workers=%u load=%s voices=%u "
                "cpuThreads=%u cpuDuty=%u seconds=%.3f\n",
                options.workers, LoadName(options.load), spec.voices,
                options.cpuThreads, options.cpuDuty, options.seconds);
    std::printf("device: WASAPI shared, %u Hz, %u channels, requested "
                "period=%u frames; engine block=%u frames\n",
                kSampleRate, kDeviceChannels, kRequestedPeriodFrames,
                kEngineBlockFrames);

    while (QpcTicks() < endQpc) {
        if (Vn_Update() != VN_OK) {
            std::fprintf(stderr, "error: Vn_Update failed during measurement\n");
            return 3;
        }
        recordQueue.Drain(&callbacks);
        const std::uint64_t now = QpcTicks();
        if (now >= nextSnapshotQpc) {
            (void)CaptureSnapshot(snapshotSequence++, &snapshots);
            nextSnapshotQpc =
                now + frequency * kSnapshotPeriodMs / 1000;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    device.Stop();
    competition.Stop();
    recordQueue.Drain(&callbacks);
    (void)CaptureSnapshot(snapshotSequence, &snapshots);

    const std::uint64_t dropped = recordQueue.Dropped();
    if (!WriteCsv(csv, callbacks, snapshots, frequency, options)) {
        std::fprintf(stderr, "error: failed while writing CSV '%s'\n",
                     options.csvPath.c_str());
        return 1;
    }
    PrintSummary(callbacks, snapshots, frequency, dropped);
    std::printf("csv: %s\n", options.csvPath.c_str());
    if (dropped != 0) {
        std::fprintf(stderr,
                     "error: callback record queue overflowed; CSV is "
                     "incomplete\n");
        return 3;
    }
    return 0;
}

#endif

} // namespace

void PrintSchedulingMeasureUsage() {
    std::fprintf(
        stderr,
        "\nsched-measure records real WASAPI callback timing and periodic "
        "engine snapshots:\n"
        "  harken_host sched-measure --workers <0|1|2|4|8> "
        "--load <light|medium|heavy>\n"
        "      --seconds <N> --csv <path> "
        "[--cpu-threads <0..256>] --cpu-duty <0|50|90>\n"
        "  Baseline uses --cpu-threads 0 --cpu-duty 0. A nonzero duty "
        "requires a nonzero thread count.\n");
}

int RunSchedulingMeasure(int argc, char** argv) {
    SchedulingOptions options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintSchedulingMeasureUsage();
        return 1;
    }
#if defined(_WIN32)
    return RunWindowsMeasurement(options);
#else
    std::fprintf(stderr,
                 "error: sched-measure is available only on Windows/WASAPI\n");
    return 1;
#endif
}

} // namespace harkenhost
