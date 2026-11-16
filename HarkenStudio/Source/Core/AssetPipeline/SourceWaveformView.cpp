#include "SourceWaveformView.h"

#include "Core/SoundSourceSemantics.h"

namespace VNX {

SourceWaveformView::SourceWaveformView(const SoundSourceDef& source, uint64_t totalFrames)
    : source_(SoundSourceSemantics::canonicalized(source, totalFrames))
{
}

uint64_t SourceWaveformView::startFrame() const
{
    return source_.trimStartFrame;
}

uint64_t SourceWaveformView::endFrame() const
{
    return *source_.trimEndFrame;
}

double SourceWaveformView::gainAtFrame(double absoluteFrame) const
{
    return SoundSourceSemantics::fadeGainAtFrame(source_, absoluteFrame);
}

}
