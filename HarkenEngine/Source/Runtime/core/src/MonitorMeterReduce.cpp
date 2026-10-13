#include "harken/core/MonitorMeterReduce.h"
#include "harken/core/ChannelLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>

namespace harken {
namespace core {

namespace {

constexpr double kLoudnessShelfFrequency = 1681.974450955533;

// Four distinct 12-tap polyphase kernels for 4x interpolation (0/0.25/0.5/0.75).
// Windowed-sinc prototype, DC-normalized so each phase sums to 1.
constexpr float kTruePeakPhase[kMeterTruePeakPhases][kMeterTruePeakTaps] = {
    {-0.000000000f, 0.001743332f, -0.008160553f, 0.021883208f, -0.049940445f,
     0.131675887f, 0.972991149f, -0.098730544f, 0.040708894f, -0.017531554f,
     0.006004166f, -0.000942601f},
    {-0.000233139f, 0.006838826f, -0.025973079f, 0.065304285f, -0.148842873f,
     0.457315029f, 0.776024267f, -0.186169482f, 0.080220157f, -0.033453296f,
     0.010239941f, -0.000971576f},
    {-0.000971576f, 0.010239941f, -0.033453296f, 0.080220157f, -0.186169482f,
     0.776024267f, 0.457315029f, -0.148842873f, 0.065304285f, -0.025973079f,
     0.006838826f, -0.000233139f},
    {-0.000942601f, 0.006004166f, -0.017531554f, 0.040708894f, -0.098730544f,
     0.972991149f, 0.131675887f, -0.049940445f, 0.021883208f, -0.008160553f,
     0.001743332f, -0.000000000f}};

float MixSample(const float* interleaved, std::uint32_t frame,
                std::uint32_t channels, std::uint32_t channelMode) {
    if (channelMode == VN_MONITOR_CHANNEL_MONO_AVERAGE) {
        double sum = 0.0;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            sum += static_cast<double>(interleaved[frame * channels + ch]);
        }
        return static_cast<float>(sum / static_cast<double>(channels));
    }
    float peak = 0.0f;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        const float absSample = std::fabs(interleaved[frame * channels + ch]);
        if (absSample > peak) {
            peak = absSample;
        }
    }
    return peak;
}

float ConvolvePhase(const MonitorMeterState& state, std::uint32_t phase) {
    float acc = 0.0f;
    for (std::uint32_t tap = 0; tap < kMeterTruePeakTaps; ++tap) {
        const std::uint32_t index =
            (state.truePeakWrite + kMeterTruePeakTaps - 1u - tap) %
            kMeterTruePeakTaps;
        acc += state.truePeakHistory[index] * kTruePeakPhase[phase][tap];
    }
    return acc;
}

void EnsureRmsBuffer(MonitorMeterState* state) {
    if (state->rmsSquares != nullptr || state->rmsWindowSamples == 0) {
        return;
    }
    state->rmsSquares.reset(new float[kMeterMaxRmsSamples]());
}

void PushRms(MonitorMeterState* state, float square) {
    EnsureRmsBuffer(state);
    if (state->rmsSquares == nullptr || state->rmsWindowSamples == 0) {
        return;
    }
    if (state->rmsCount == state->rmsWindowSamples) {
        const float old = state->rmsSquares[state->rmsWrite];
        state->rmsSumSquares -= static_cast<double>(old);
        state->rmsSquares[state->rmsWrite] = square;
        state->rmsSumSquares += static_cast<double>(square);
        if (state->rmsSumSquares < 0.0) {
            state->rmsSumSquares = 0.0;
        }
        ++state->rmsWrite;
        if (state->rmsWrite == state->rmsWindowSamples) {
            state->rmsWrite = 0;
        }
        state->rmsReady = true;
        return;
    }
    state->rmsSquares[state->rmsCount] = square;
    state->rmsSumSquares += static_cast<double>(square);
    ++state->rmsCount;
    if (state->rmsCount == state->rmsWindowSamples) {
        state->rmsWrite = 0;
        state->rmsReady = true;
    }
}

void TickHold(MonitorMeterState* state) {
    if (!state->holdReady) {
        return;
    }
    if (state->holdRemain > 0) {
        --state->holdRemain;
        return;
    }
    if (state->sampleRate == 0) {
        return;
    }
    const float factor = std::pow(
        10.0f, -1.0f / static_cast<float>(state->sampleRate));
    state->holdLinear *= factor;
    if (state->holdLinear < 1.0e-8f) {
        state->holdLinear = 0.0f;
    }
}

} // namespace

VnResult ValidateMeterRequest(const VnMonitorMeasurementRequest& request) {
    if (request.structSize != sizeof(VnMonitorMeasurementRequest)) {
        return VN_INVALID_ARG;
    }
    if (request.kind == VN_MONITOR_MEASURE_LUFS_SHORT_TERM ||
        request.kind == VN_MONITOR_MEASURE_LUFS_INTEGRATED) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.kind != VN_MONITOR_MEASURE_SAMPLE_PEAK &&
        request.kind != VN_MONITOR_MEASURE_TRUE_PEAK &&
        request.kind != VN_MONITOR_MEASURE_RMS &&
        request.kind != VN_MONITOR_MEASURE_LUFS_MOMENTARY &&
        request.kind != VN_MONITOR_MEASURE_PEAK_HOLD &&
        request.kind != VN_MONITOR_MEASURE_GAIN_REDUCTION) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.downsample != 0 && request.downsample != 1) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.channelMode != VN_MONITOR_CHANNEL_PER_CHANNEL &&
        request.channelMode != VN_MONITOR_CHANNEL_MONO_AVERAGE) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.fftSize != 0 || request.bucketFrames != 0) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.kind == VN_MONITOR_MEASURE_LUFS_MOMENTARY &&
        request.channelMode != VN_MONITOR_CHANNEL_PER_CHANNEL) {
        return VN_MONITOR_UNSUPPORTED;
    }
    return VN_OK;
}

void ResetMeterState(MonitorMeterState* state, std::uint32_t sampleRate,
                     std::uint64_t generation) {
    if (state == nullptr) {
        return;
    }
    state->sampleRate = sampleRate;
    state->segmentGeneration = generation;
    state->samplePeak = 0.0f;
    state->truePeak = 0.0f;
    state->rmsSumSquares = 0.0;
    state->rmsCount = 0;
    state->rmsWrite = 0;
    state->holdLinear = 0.0f;
    state->holdRemain = 0;
    state->truePeakFilled = 0;
    state->truePeakWrite = 0;
    state->armedNumeric = false;
    state->rmsReady = false;
    state->truePeakReady = false;
    state->holdReady = false;
    state->truePeakHistory.fill(0.0f);
    state->rmsWindowSamples = 0;
    state->holdSamples = 0;
    if (sampleRate != 0) {
        std::uint32_t rmsSamples = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(sampleRate) * 300ull) / 1000ull);
        if (rmsSamples == 0) {
            rmsSamples = 1;
        }
        if (rmsSamples > kMeterMaxRmsSamples) {
            rmsSamples = kMeterMaxRmsSamples;
        }
        state->rmsWindowSamples = rmsSamples;
        state->holdSamples = sampleRate;
        if (state->holdSamples == 0) {
            state->holdSamples = 1;
        }
        if (state->rmsSquares != nullptr) {
            std::memset(state->rmsSquares.get(), 0,
                        sizeof(float) * kMeterMaxRmsSamples);
        }
    }
}

void NoteMeterSegment(MonitorMeterState* state, std::uint64_t generation) {
    if (state == nullptr || state->segmentGeneration == generation) {
        return;
    }
    ResetMeterState(state, state->sampleRate, generation);
}

MonitorMeterChunkResult ReduceMeterChunk(MonitorMeterState* state,
                                         const float* interleaved,
                                         std::uint32_t frames,
                                         std::uint32_t channels,
                                         std::uint32_t channelMode) {
    MonitorMeterChunkResult result{};
    if (state == nullptr || interleaved == nullptr || frames == 0 ||
        channels == 0) {
        result.invalid = true;
        return result;
    }
    state->samplePeak = 0.0f;
    state->truePeak = 0.0f;
    bool sawFinite = false;
    for (std::uint32_t i = 0; i < frames; ++i) {
        bool frameFinite = true;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            if (!std::isfinite(interleaved[i * channels + ch])) {
                frameFinite = false;
                break;
            }
        }
        if (!frameFinite) {
            result.invalid = true;
            state->armedNumeric = false;
            continue;
        }
        sawFinite = true;
        const float mixed = MixSample(interleaved, i, channels, channelMode);
        const float absSample = std::fabs(mixed);
        if (absSample > state->samplePeak) {
            state->samplePeak = absSample;
        }
        double rmsSquare = 0.0;
        if (channelMode == VN_MONITOR_CHANNEL_MONO_AVERAGE) {
            rmsSquare = static_cast<double>(mixed) * mixed;
        } else {
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                const double sample = interleaved[i * channels + channel];
                rmsSquare += sample * sample;
            }
            rmsSquare /= channels;
        }
        PushRms(state, static_cast<float>(rmsSquare));
        if (!state->holdReady || absSample >= state->holdLinear) {
            state->holdLinear = absSample;
            state->holdRemain = state->holdSamples;
            state->holdReady = true;
        } else {
            TickHold(state);
        }
        state->truePeakHistory[state->truePeakWrite] = mixed;
        state->truePeakWrite = (state->truePeakWrite + 1u) % kMeterTruePeakTaps;
        if (state->truePeakFilled < kMeterTruePeakTaps) {
            ++state->truePeakFilled;
        }
        if (state->truePeakFilled >= kMeterTruePeakWarmup) {
            state->truePeakReady = true;
            for (std::uint32_t phase = 0; phase < kMeterTruePeakPhases; ++phase) {
                const float phasePeak = std::fabs(ConvolvePhase(*state, phase));
                if (phasePeak > state->truePeak) {
                    state->truePeak = phasePeak;
                }
            }
        }
        state->armedNumeric = true;
    }
    if (!sawFinite) {
        result.invalid = true;
        return result;
    }
    result.samplePeak = state->samplePeak;
    result.truePeak = state->truePeakReady ? state->truePeak : 0.0f;
    result.rmsLinear =
        state->rmsReady
            ? std::sqrt(static_cast<float>(
                  state->rmsSumSquares /
                  static_cast<double>(state->rmsWindowSamples)))
            : 0.0f;
    result.holdLinear = state->holdReady ? state->holdLinear : 0.0f;
    result.numeric = state->armedNumeric;
    result.rmsWarmup = !state->rmsReady;
    result.truePeakWarmup = !state->truePeakReady;
    return result;
}

bool LoudnessSampleRateSupported(std::uint32_t sampleRate) {
    return sampleRate > 2.0 * kLoudnessShelfFrequency && sampleRate <= 384000;
}

void ResetLoudnessState(MonitorLoudnessState* state, std::uint32_t sampleRate,
                        std::uint32_t layout, std::uint64_t generation) {
    if (state == nullptr) {
        return;
    }
    state->sampleRate = sampleRate;
    state->layout = layout;
    state->channels = LayoutChannelCount(layout);
    state->segmentGeneration = generation;
    state->count = state->write = 0;
    state->sumSquares = 0.0;
    state->history = {};
    state->weights = {};
    state->squares.clear();
    if (!LoudnessSampleRateSupported(sampleRate) || state->channels == 0) {
        return;
    }
    state->squares.assign(static_cast<std::size_t>(sampleRate) * 400 / 1000, 0.0);
    std::array<ChannelRole, 8> roles{};
    LayoutRoles(layout, roles.data());
    for (std::uint32_t channel = 0; channel < state->channels; ++channel) {
        const auto role = roles[channel];
        state->weights[channel] = role == ChannelRole::LFE ? 0.0
            : role == ChannelRole::BL || role == ChannelRole::BR ||
                role == ChannelRole::SL || role == ChannelRole::SR ? 1.41 : 1.0;
    }
    const double shelfK = std::tan(std::numbers::pi * kLoudnessShelfFrequency / sampleRate);
    const double shelfQ = 0.7071752369554196;
    const double highGain = std::pow(10.0, 3.999843853973347 / 20.0);
    const double middleGain = std::pow(highGain, 0.4996667741545416);
    const double shelfA0 = 1.0 + shelfK / shelfQ + shelfK * shelfK;
    state->coefficients[0] = {
        (highGain + middleGain * shelfK / shelfQ + shelfK * shelfK) / shelfA0,
        2.0 * (shelfK * shelfK - highGain) / shelfA0,
        (highGain - middleGain * shelfK / shelfQ + shelfK * shelfK) / shelfA0,
        2.0 * (shelfK * shelfK - 1.0) / shelfA0,
        (1.0 - shelfK / shelfQ + shelfK * shelfK) / shelfA0};
    const double highPassK = std::tan(std::numbers::pi * 38.13547087602444 / sampleRate);
    const double highPassQ = 0.5003270373238773;
    const double highPassA0 = 1.0 + highPassK / highPassQ + highPassK * highPassK;
    state->coefficients[1] = {1.0, -2.0, 1.0,
        2.0 * (highPassK * highPassK - 1.0) / highPassA0,
        (1.0 - highPassK / highPassQ + highPassK * highPassK) / highPassA0};
}

MonitorMeterChunkResult ReduceLoudnessChunk(MonitorLoudnessState* state,
                                            const float* interleaved,
                                            std::uint32_t frames,
                                            std::uint32_t channels) {
    MonitorMeterChunkResult result{};
    if (state == nullptr || interleaved == nullptr || frames == 0 ||
        channels != state->channels || state->squares.empty()) {
        result.invalid = true;
        return result;
    }
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        double energy = 0.0;
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            double sample = interleaved[frame * channels + channel];
            if (!std::isfinite(sample)) {
                ResetLoudnessState(state, state->sampleRate, state->layout,
                                   state->segmentGeneration);
                result.invalid = true;
                return result;
            }
            if (state->weights[channel] == 0.0) {
                continue;
            }
            auto& history = state->history[channel];
            for (std::size_t stage = 0; stage < 2; ++stage) {
                const auto& c = state->coefficients[stage];
                const double filtered = c[0] * sample + history[stage * 2];
                history[stage * 2] = c[1] * sample - c[3] * filtered + history[stage * 2 + 1];
                history[stage * 2 + 1] = c[2] * sample - c[4] * filtered;
                sample = filtered;
            }
            energy += state->weights[channel] * sample * sample;
        }
        state->sumSquares += energy - state->squares[state->write];
        state->squares[state->write] = energy;
        state->write = (state->write + 1) % state->squares.size();
        state->count = std::min(state->count + 1, static_cast<std::uint32_t>(state->squares.size()));
    }
    result.numeric = true;
    result.loudnessWarmup = state->count != state->squares.size();
    result.loudnessMomentary = state->sumSquares > 0.0
        ? static_cast<float>(-0.691 + 10.0 * std::log10(state->sumSquares / state->squares.size()))
        : -std::numeric_limits<float>::infinity();
    return result;
}

float MeterLinearToDb(float linear) {
    if (!(linear > 0.0f) || !std::isfinite(linear)) {
        return kMeterMinus160Db;
    }
    return 20.0f * std::log10(linear);
}

std::uint32_t MeterFloatBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool MeterHasIdentity(const VnMonitorTapDescriptor& tap) {
    return tap.ownerStableId != VnObjectId{} && tap.ownerInstanceHandle != 0 &&
           tap.ownerGeneration != 0;
}

MonitorMeterGainReductionSample ResolveMeterGainReduction(
    const VnMonitorTapDescriptor& tap, MonitorMeterGainReductionLookup lookup,
    void* ctx) {
    MonitorMeterGainReductionSample sample{};
    if (!MeterHasIdentity(tap) || lookup == nullptr) {
        return sample;
    }
    sample = lookup(tap, ctx);
    if (!sample.valid || !std::isfinite(sample.reductionDb) ||
        sample.reductionDb < 0.0f || sample.ownerStableId == VnObjectId{} ||
        sample.ownerInstanceHandle == 0 || sample.ownerGeneration == 0) {
        return MonitorMeterGainReductionSample{};
    }
    return sample;
}

} // namespace core
} // namespace harken
