#include "WaveformBucketProjection.h"

namespace VNX {
namespace Monitor {

bool waveformBucketsPublished()
{
    return false;
}

WaveformBucketProjection projectWaveformFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot)
{
    WaveformBucketProjection projection;
    projection.face = WaveformFace::Unpublished;
    projection.gapId = "P5-2";
    projection.generation = snapshot ? snapshot->generation : 0;
    projection.bucketsPublished = waveformBucketsPublished();
    return projection;
}

} // namespace Monitor
} // namespace VNX
