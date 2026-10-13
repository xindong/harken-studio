#pragma once

#include "MonitorStore.h"

#include <cstdint>
#include <memory>

namespace VNX {
namespace Monitor {

enum class WaveformFace
{
    Unpublished = 0,
};

// Empty-state face only. snapshot is host generation plumbing.
// This pack never reads WAVEFORM_BUCKET, recordKind, or payload.
// waveformBucketsPublished stays false until a later pack publishes bytes.
struct WaveformBucketProjection
{
    WaveformFace face = WaveformFace::Unpublished;
    const char* gapId = "P5-2";
    std::uint64_t generation = 0;
    bool bucketsPublished = false;
};

bool waveformBucketsPublished();
WaveformBucketProjection projectWaveformFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot);

} // namespace Monitor
} // namespace VNX
