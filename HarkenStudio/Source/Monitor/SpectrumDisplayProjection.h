#pragma once

#include "MonitorStore.h"

#include <cstdint>
#include <memory>

namespace VNX {
namespace Monitor {

enum class SpectrumFace
{
    Unpublished = 0,
};

// Empty-state face only. snapshot is host generation plumbing.
// This pack never reads SPECTRUM, recordKind, bins, fftSize, or capability.
// spectrumBinsPublished stays false until a later pack publishes bytes.
struct SpectrumDisplayProjection
{
    SpectrumFace face = SpectrumFace::Unpublished;
    const char* gapId = "P5-4";
    std::uint64_t generation = 0;
    bool binsPublished = false;
};

bool spectrumBinsPublished();
SpectrumDisplayProjection projectSpectrumFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot);

} // namespace Monitor
} // namespace VNX
