#include "harken/core/MonitorSpectrumFft.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace harken {
namespace core {

namespace {

constexpr float kPi = 3.14159265358979323846f;

void BitReverse(float* data, std::uint32_t n) {
    std::uint32_t j = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (i < j) {
            const float tmpRe = data[2 * i];
            const float tmpIm = data[2 * i + 1];
            data[2 * i] = data[2 * j];
            data[2 * i + 1] = data[2 * j + 1];
            data[2 * j] = tmpRe;
            data[2 * j + 1] = tmpIm;
        }
        std::uint32_t m = n >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }
}

} // namespace

bool AllowedSpectrumFftSize(std::uint32_t fftSize) {
    return fftSize == 512 || fftSize == 1024 || fftSize == 2048 || fftSize == 4096;
}

std::uint32_t SpectrumHop(std::uint32_t fftSize) { return fftSize / 2u; }

std::uint32_t SpectrumBinCount(std::uint32_t fftSize) { return fftSize / 2u + 1u; }

VnResult ValidateSpectrumRequest(const VnMonitorMeasurementRequest& request) {
    if (request.structSize != sizeof(VnMonitorMeasurementRequest)) {
        return VN_INVALID_ARG;
    }
    if (request.kind != VN_MONITOR_MEASURE_SPECTRUM) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.downsample != 1) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.windowKind != 0) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.channelMode != VN_MONITOR_CHANNEL_PER_CHANNEL &&
        request.channelMode != VN_MONITOR_CHANNEL_MONO_AVERAGE) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.reserved0 != 0) {
        return VN_INVALID_ARG;
    }
    if (!AllowedSpectrumFftSize(request.fftSize)) {
        return VN_MONITOR_UNSUPPORTED;
    }
    const bool defaultOverlap =
        request.overlapNumerator == 0 && request.overlapDenominator == 0;
    const bool halfOverlap =
        request.overlapNumerator * 2u == request.overlapDenominator &&
        request.overlapDenominator != 0;
    if (!defaultOverlap && !halfOverlap) {
        return VN_MONITOR_UNSUPPORTED;
    }
    return VN_OK;
}

void HannWindow(float* samples, std::uint32_t count) {
    if (samples == nullptr || count < 2) {
        return;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const float w =
            0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) /
                                   static_cast<float>(count));
        samples[i] *= w;
    }
}

void RealForwardFft(float* data, std::uint32_t n) {
    if (data == nullptr || n < 2) {
        return;
    }
    BitReverse(data, n);
    for (std::uint32_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * kPi / static_cast<float>(len);
        const float wLenRe = std::cos(ang);
        const float wLenIm = std::sin(ang);
        for (std::uint32_t i = 0; i < n; i += len) {
            float wRe = 1.0f;
            float wIm = 0.0f;
            for (std::uint32_t j = 0; j < len / 2; ++j) {
                const std::uint32_t even = 2 * (i + j);
                const std::uint32_t odd = 2 * (i + j + len / 2);
                const float tRe = wRe * data[odd] - wIm * data[odd + 1];
                const float tIm = wRe * data[odd + 1] + wIm * data[odd];
                data[odd] = data[even] - tRe;
                data[odd + 1] = data[even + 1] - tIm;
                data[even] += tRe;
                data[even + 1] += tIm;
                const float nextRe = wRe * wLenRe - wIm * wLenIm;
                wIm = wRe * wLenIm + wIm * wLenRe;
                wRe = nextRe;
            }
        }
    }
}

void SpectrumMagnitudeDb(const float* fft, std::uint32_t fftSize, float* outBins,
                         std::uint32_t binCount) {
    if (fft == nullptr || outBins == nullptr || binCount == 0 || fftSize == 0) {
        return;
    }
    const float scale = 2.0f / static_cast<float>(fftSize);
    for (std::uint32_t k = 0; k < binCount; ++k) {
        const float re = fft[2 * k];
        const float im = fft[2 * k + 1];
        float mag = std::sqrt(re * re + im * im) * scale;
        if (k == 0 || (binCount > 1 && k + 1 == binCount && (fftSize % 2u) == 0)) {
            mag = std::sqrt(re * re + im * im) / static_cast<float>(fftSize);
        }
        float db = 20.0f * std::log10(mag + 1.0e-20f);
        if (db < kMonitorSpectrumFloorDb) {
            db = kMonitorSpectrumFloorDb;
        }
        outBins[k] = db;
    }
}

void ResetSpectrumState(MonitorSpectrumState* state, std::uint32_t fftSize,
                        std::uint64_t generation) {
    if (state == nullptr) {
        return;
    }
    *state = MonitorSpectrumState{};
    state->fftSize = fftSize;
    state->segmentGeneration = generation;
}

void AnalyzeHannSpectrum(const float* time, std::uint32_t fftSize, float* workTime,
                         float* workFft, float* outBins) {
    if (time == nullptr || workTime == nullptr || workFft == nullptr ||
        outBins == nullptr || !AllowedSpectrumFftSize(fftSize)) {
        return;
    }
    std::memcpy(workTime, time, static_cast<std::size_t>(fftSize) * sizeof(float));
    HannWindow(workTime, fftSize);
    for (std::uint32_t i = 0; i < fftSize; ++i) {
        workTime[i] *= kMonitorSpectrumHannAmplitudeGain;
    }
    std::memset(workFft, 0, static_cast<std::size_t>(fftSize) * 2u * sizeof(float));
    for (std::uint32_t i = 0; i < fftSize; ++i) {
        workFft[2 * i] = workTime[i];
    }
    RealForwardFft(workFft, fftSize);
    SpectrumMagnitudeDb(workFft, fftSize, outBins, SpectrumBinCount(fftSize));
}

bool PushSpectrumSample(MonitorSpectrumState* state, float sample,
                        std::uint64_t frame, float* workTime, float* workFft,
                        MonitorSpectrumWindow* out) {
    if (state == nullptr || out == nullptr || workTime == nullptr ||
        workFft == nullptr || !AllowedSpectrumFftSize(state->fftSize)) {
        return false;
    }
    *out = MonitorSpectrumWindow{};
    if (!std::isfinite(sample)) {
        state->filled = 0;
        state->bufferStartFrame = 0;
        return false;
    }
    if (state->filled == 0) {
        state->bufferStartFrame = frame;
    } else if (frame != state->bufferStartFrame + state->filled) {
        state->filled = 0;
        state->bufferStartFrame = frame;
    }
    state->buffer[state->filled++] = sample;
    if (state->filled < state->fftSize) {
        return false;
    }
    out->complete = true;
    out->sourceFrameBegin = state->bufferStartFrame;
    out->sourceFrameEnd = state->bufferStartFrame + state->fftSize;
    out->binCount = SpectrumBinCount(state->fftSize);
    AnalyzeHannSpectrum(state->buffer, state->fftSize, workTime, workFft,
                        out->bins);
    const std::uint32_t hop = SpectrumHop(state->fftSize);
    std::memmove(state->buffer, state->buffer + hop,
                 static_cast<std::size_t>(state->fftSize - hop) * sizeof(float));
    state->filled = hop;
    state->bufferStartFrame += hop;
    return true;
}

} // namespace core
} // namespace harken
