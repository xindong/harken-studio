#ifndef HARKEN_CORE_MONITOR_METER_REDUCE_H
#define HARKEN_CORE_MONITOR_METER_REDUCE_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace harken {
namespace core {

inline constexpr std::uint32_t kMeterTruePeakPhases = 4;
inline constexpr std::uint32_t kMeterTruePeakTaps = 12;
inline constexpr std::uint32_t kMeterTruePeakWarmup = kMeterTruePeakTaps;
inline constexpr std::uint32_t kMeterAlgorithmVersion = 1;
inline constexpr std::uint32_t kMeterUnitLinear = 1;
inline constexpr std::uint32_t kMeterUnitDbfs = 2;
inline constexpr std::uint32_t kMeterUnitDbtp = 3;
inline constexpr std::uint32_t kMeterUnitDb = 4;
inline constexpr std::uint32_t kMeterMaxRmsSamples = 192000;
inline constexpr float kMeterMinus160Db = -160.0f;

struct MonitorMeterGainReductionSample {
    bool valid = false;
    float reductionDb = 0.0f;
    VnObjectId ownerStableId{};
    std::uint64_t ownerInstanceHandle = 0;
    std::uint64_t ownerGeneration = 0;
};

using MonitorMeterGainReductionLookup =
    MonitorMeterGainReductionSample (*)(const VnMonitorTapDescriptor& tap,
                                        void* ctx);

struct MonitorMeterState {
    std::uint32_t sampleRate = 0;
    std::uint32_t rmsWindowSamples = 0;
    std::uint32_t holdSamples = 0;
    float samplePeak = 0.0f;
    float truePeak = 0.0f;
    double rmsSumSquares = 0.0;
    std::uint32_t rmsCount = 0;
    std::uint32_t rmsWrite = 0;
    float holdLinear = 0.0f;
    std::uint32_t holdRemain = 0;
    std::uint32_t truePeakFilled = 0;
    std::uint32_t truePeakWrite = 0;
    std::uint64_t segmentGeneration = 0;
    bool armedNumeric = false;
    bool rmsReady = false;
    bool truePeakReady = false;
    bool holdReady = false;
    std::unique_ptr<float[]> rmsSquares;
    std::array<float, kMeterTruePeakTaps> truePeakHistory{};
};

struct MonitorMeterChunkResult {
    bool numeric = false;
    bool rmsWarmup = false;
    bool truePeakWarmup = false;
    bool loudnessWarmup = false;
    bool invalid = false;
    float samplePeak = 0.0f;
    float truePeak = 0.0f;
    float rmsLinear = 0.0f;
    float holdLinear = 0.0f;
    float loudnessMomentary = 0.0f;
};

struct MonitorLoudnessState {
    std::uint32_t sampleRate = 0;
    std::uint32_t layout = 0;
    std::uint32_t channels = 0;
    std::uint32_t count = 0;
    std::uint32_t write = 0;
    std::uint64_t segmentGeneration = 0;
    double sumSquares = 0.0;
    std::array<std::array<double, 5>, 2> coefficients{};
    std::array<std::array<double, 4>, 8> history{};
    std::array<double, 8> weights{};
    std::vector<double> squares;
};

VnResult ValidateMeterRequest(const VnMonitorMeasurementRequest& request);
bool LoudnessSampleRateSupported(std::uint32_t sampleRate);
void ResetMeterState(MonitorMeterState* state, std::uint32_t sampleRate,
                     std::uint64_t generation);
void NoteMeterSegment(MonitorMeterState* state, std::uint64_t generation);
MonitorMeterChunkResult ReduceMeterChunk(MonitorMeterState* state,
                                         const float* interleaved,
                                         std::uint32_t frames,
                                         std::uint32_t channels,
                                         std::uint32_t channelMode);
void ResetLoudnessState(MonitorLoudnessState* state, std::uint32_t sampleRate,
                        std::uint32_t layout, std::uint64_t generation);
MonitorMeterChunkResult ReduceLoudnessChunk(MonitorLoudnessState* state,
                                            const float* interleaved,
                                            std::uint32_t frames,
                                            std::uint32_t channels);
float MeterLinearToDb(float linear);
std::uint32_t MeterFloatBits(float value);
bool MeterHasIdentity(const VnMonitorTapDescriptor& tap);
MonitorMeterGainReductionSample ResolveMeterGainReduction(
    const VnMonitorTapDescriptor& tap, MonitorMeterGainReductionLookup lookup,
    void* ctx);

} // namespace core
} // namespace harken

#endif
