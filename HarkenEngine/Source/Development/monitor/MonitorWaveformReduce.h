#ifndef HARKEN_CORE_MONITOR_WAVEFORM_REDUCE_H
#define HARKEN_CORE_MONITOR_WAVEFORM_REDUCE_H

#include <harken/Harken.h>

#include <cstdint>

namespace harken {
namespace core {

struct MonitorWaveformBucketResult {
    bool complete = false;
    bool discarded = false;
    float minValue = 0.0f;
    float maxValue = 0.0f;
    std::uint32_t validSampleCount = 0;
    std::uint64_t sourceFrameBegin = 0;
    std::uint64_t sourceFrameEnd = 0;
    std::uint64_t sourceSequenceFirst = 0;
    std::uint64_t sourceSequenceLast = 0;
};

struct MonitorWaveformReducer {
    std::uint32_t bucketFrames = 0;
    std::uint32_t channelMode = VN_MONITOR_CHANNEL_PER_CHANNEL;
    std::uint64_t segmentGeneration = 0;
    std::uint64_t filledBegin = 0;
    std::uint32_t filledCount = 0;
    bool hasMinMax = false;
    float minValue = 0.0f;
    float maxValue = 0.0f;
    std::uint64_t sourceSequenceFirst = 0;
    std::uint64_t sourceSequenceLast = 0;

    void Reset(std::uint32_t frames, std::uint32_t mode,
               std::uint64_t generation) {
        bucketFrames = frames;
        channelMode = mode;
        segmentGeneration = generation;
        filledBegin = 0;
        filledCount = 0;
        hasMinMax = false;
        minValue = 0.0f;
        maxValue = 0.0f;
        sourceSequenceFirst = 0;
        sourceSequenceLast = 0;
    }
};

inline constexpr std::uint32_t kWaveformBucketResultCap =
    VN_MONITOR_HARD_AUDIO_CHUNK_FRAMES;

VnResult ValidateWaveformRequest(const VnMonitorMeasurementRequest& request);
void ObserveWaveformSample(MonitorWaveformReducer* reducer, float sample);
MonitorWaveformBucketResult FlushWaveformPartial(MonitorWaveformReducer* reducer);
void NoteWaveformSegment(MonitorWaveformReducer* reducer,
                         std::uint64_t generation,
                         MonitorWaveformBucketResult* discarded);

bool ReduceWaveformChunk(MonitorWaveformReducer* reducer,
                         const float* interleaved, std::uint32_t frames,
                         std::uint32_t channels, std::uint64_t sourceFrameBegin,
                         MonitorWaveformBucketResult* out, std::uint32_t outCap,
                         std::uint32_t* outCount,
                         std::uint64_t sourceSequence = 0);

} // namespace core
} // namespace harken

#endif
