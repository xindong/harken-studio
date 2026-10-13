#include "harken/core/MonitorWaveformReduce.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace harken {
namespace core {

namespace {

float MixMonoAverage(const float* interleaved, std::uint32_t frame,
                     std::uint32_t channels) {
    double sum = 0.0;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        sum += static_cast<double>(interleaved[frame * channels + ch]);
    }
    const double mean = sum / static_cast<double>(channels);
    return static_cast<float>(mean);
}

void IncludeSample(MonitorWaveformReducer* reducer, float sample) {
    if (!reducer->hasMinMax) {
        reducer->minValue = sample;
        reducer->maxValue = sample;
        reducer->hasMinMax = true;
        return;
    }
    if (sample < reducer->minValue) {
        reducer->minValue = sample;
    }
    if (sample > reducer->maxValue) {
        reducer->maxValue = sample;
    }
}

std::uint64_t BucketBegin(std::uint64_t frame, std::uint32_t bucketFrames) {
    return (frame / bucketFrames) * static_cast<std::uint64_t>(bucketFrames);
}

} // namespace

VnResult ValidateWaveformRequest(const VnMonitorMeasurementRequest& request) {
    if (request.structSize != sizeof(VnMonitorMeasurementRequest)) {
        return VN_INVALID_ARG;
    }
    if (request.kind != VN_MONITOR_MEASURE_WAVEFORM) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.downsample != 0 && request.downsample != 1) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.channelMode != VN_MONITOR_CHANNEL_PER_CHANNEL &&
        request.channelMode != VN_MONITOR_CHANNEL_MONO_AVERAGE) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.bucketFrames == 0) {
        return VN_INVALID_ARG;
    }
    return VN_OK;
}

void ObserveWaveformSample(MonitorWaveformReducer* reducer, float sample) {
    if (reducer == nullptr || !std::isfinite(sample)) {
        return;
    }
    IncludeSample(reducer, sample);
}

MonitorWaveformBucketResult FlushWaveformPartial(MonitorWaveformReducer* reducer) {
    MonitorWaveformBucketResult result{};
    if (reducer == nullptr || reducer->bucketFrames == 0 ||
        reducer->filledCount == 0) {
        return result;
    }
    result.discarded = true;
    result.validSampleCount = reducer->filledCount;
    result.sourceFrameBegin = reducer->filledBegin;
    result.sourceFrameEnd = reducer->filledBegin + reducer->filledCount;
    result.sourceSequenceFirst = reducer->sourceSequenceFirst;
    result.sourceSequenceLast = reducer->sourceSequenceLast;
    reducer->filledCount = 0;
    reducer->hasMinMax = false;
    reducer->minValue = 0.0f;
    reducer->maxValue = 0.0f;
    reducer->sourceSequenceFirst = 0;
    reducer->sourceSequenceLast = 0;
    return result;
}

void NoteWaveformSegment(MonitorWaveformReducer* reducer,
                         std::uint64_t generation,
                         MonitorWaveformBucketResult* discarded) {
    if (reducer == nullptr) {
        return;
    }
    if (reducer->segmentGeneration != generation && reducer->filledCount != 0 &&
        discarded != nullptr) {
        *discarded = FlushWaveformPartial(reducer);
    } else if (reducer->segmentGeneration != generation) {
        reducer->filledCount = 0;
        reducer->hasMinMax = false;
    }
    reducer->segmentGeneration = generation;
}

bool ReduceWaveformChunk(MonitorWaveformReducer* reducer,
                         const float* interleaved, std::uint32_t frames,
                         std::uint32_t channels, std::uint64_t sourceFrameBegin,
                         MonitorWaveformBucketResult* out, std::uint32_t outCap,
                         std::uint32_t* outCount, std::uint64_t sourceSequence) {
    if (outCount != nullptr) {
        *outCount = 0;
    }
    if (reducer == nullptr || interleaved == nullptr || frames == 0 ||
        channels == 0 || reducer->bucketFrames == 0 || out == nullptr ||
        outCap == 0) {
        return false;
    }
    std::uint32_t written = 0;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::uint64_t frame = sourceFrameBegin + i;
        const std::uint64_t begin = BucketBegin(frame, reducer->bucketFrames);
        const bool crossing =
            reducer->filledCount != 0 && begin != reducer->filledBegin;
        const bool completingThisFrame =
            !crossing && reducer->filledCount + 1u == reducer->bucketFrames;
        if ((crossing || completingThisFrame) && written >= outCap) {
            if (outCount != nullptr) {
                *outCount = written;
            }
            return false;
        }
        if (crossing) {
            out[written] = FlushWaveformPartial(reducer);
            ++written;
            if (written >= outCap) {
                if (outCount != nullptr) {
                    *outCount = written;
                }
                return false;
            }
        }
        if (reducer->filledCount == 0) {
            reducer->filledBegin = begin;
            reducer->sourceSequenceFirst = sourceSequence;
        }
        if (reducer->channelMode == VN_MONITOR_CHANNEL_MONO_AVERAGE) {
            IncludeSample(reducer, MixMonoAverage(interleaved, i, channels));
        } else {
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                IncludeSample(reducer, interleaved[i * channels + ch]);
            }
        }
        ++reducer->filledCount;
        reducer->sourceSequenceLast = sourceSequence;
        if (reducer->filledCount == reducer->bucketFrames) {
            MonitorWaveformBucketResult complete{};
            complete.complete = true;
            complete.minValue = reducer->minValue;
            complete.maxValue = reducer->maxValue;
            complete.validSampleCount = reducer->bucketFrames;
            complete.sourceFrameBegin = reducer->filledBegin;
            complete.sourceFrameEnd =
                reducer->filledBegin + reducer->bucketFrames;
            complete.sourceSequenceFirst = reducer->sourceSequenceFirst;
            complete.sourceSequenceLast = reducer->sourceSequenceLast;
            out[written] = complete;
            ++written;
            reducer->filledCount = 0;
            reducer->hasMinMax = false;
            reducer->minValue = 0.0f;
            reducer->maxValue = 0.0f;
            reducer->sourceSequenceFirst = 0;
            reducer->sourceSequenceLast = 0;
        }
    }
    if (outCount != nullptr) {
        *outCount = written;
    }
    return true;
}

} // namespace core
} // namespace harken
