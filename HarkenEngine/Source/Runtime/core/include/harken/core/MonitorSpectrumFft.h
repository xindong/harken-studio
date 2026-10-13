#ifndef HARKEN_CORE_MONITOR_SPECTRUM_FFT_H
#define HARKEN_CORE_MONITOR_SPECTRUM_FFT_H

#include <harken/Harken.h>

#include <cstdint>

namespace harken {
namespace core {

constexpr float kMonitorSpectrumFloorDb = -160.0f;
constexpr float kMonitorSpectrumHannAmplitudeGain = 2.0f;
constexpr std::uint32_t kMonitorSpectrumMaxFft = 4096;
constexpr std::uint32_t kMonitorSpectrumMaxBins = kMonitorSpectrumMaxFft / 2u + 1u;

struct MonitorSpectrumState {
    std::uint32_t fftSize = 0;
    std::uint32_t filled = 0;
    std::uint64_t bufferStartFrame = 0;
    std::uint64_t segmentGeneration = 0;
    float buffer[kMonitorSpectrumMaxFft]{};
};

struct MonitorSpectrumWindow {
    bool complete = false;
    std::uint64_t sourceFrameBegin = 0;
    std::uint64_t sourceFrameEnd = 0;
    std::uint32_t binCount = 0;
    float bins[kMonitorSpectrumMaxBins]{};
};

bool AllowedSpectrumFftSize(std::uint32_t fftSize);
std::uint32_t SpectrumHop(std::uint32_t fftSize);
std::uint32_t SpectrumBinCount(std::uint32_t fftSize);
VnResult ValidateSpectrumRequest(const VnMonitorMeasurementRequest& request);
void HannWindow(float* samples, std::uint32_t count);
void RealForwardFft(float* data, std::uint32_t n);
void SpectrumMagnitudeDb(const float* fft, std::uint32_t fftSize, float* outBins,
                         std::uint32_t binCount);
void ResetSpectrumState(MonitorSpectrumState* state, std::uint32_t fftSize,
                        std::uint64_t generation);
void AnalyzeHannSpectrum(const float* time, std::uint32_t fftSize, float* workTime,
                         float* workFft, float* outBins);
bool PushSpectrumSample(MonitorSpectrumState* state, float sample,
                        std::uint64_t frame, float* workTime, float* workFft,
                        MonitorSpectrumWindow* out);

} // namespace core
} // namespace harken

#endif
