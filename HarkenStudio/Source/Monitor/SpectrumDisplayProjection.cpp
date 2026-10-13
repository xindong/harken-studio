#include "SpectrumDisplayProjection.h"

namespace VNX {
namespace Monitor {

bool spectrumBinsPublished()
{
    return false;
}

SpectrumDisplayProjection projectSpectrumFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot)
{
    SpectrumDisplayProjection projection;
    projection.face = SpectrumFace::Unpublished;
    projection.gapId = "P5-4";
    projection.generation = snapshot ? snapshot->generation : 0;
    projection.binsPublished = spectrumBinsPublished();
    return projection;
}

} // namespace Monitor
} // namespace VNX
