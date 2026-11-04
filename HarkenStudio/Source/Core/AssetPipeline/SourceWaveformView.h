#pragma once

#include "Core/Components/VSound.h"

namespace VNX {

class SourceWaveformView
{
public:
    explicit SourceWaveformView(const SoundSourceDef& source, uint64_t totalFrames);

    uint64_t startFrame() const;
    uint64_t endFrame() const;
    double gainAtFrame(double absoluteFrame) const;

    bool operator==(const SourceWaveformView&) const = default;

private:
    SoundSourceDef source_;
};

}
