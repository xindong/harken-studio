#include "SourceWaveformView.h"

#include "Core/CurveSemantics.h"
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
    const double start = double(startFrame());
    const double end = double(endFrame());
    if (absoluteFrame < start || absoluteFrame > end)
        return 1.0;
    const auto& fadeIn = source_.fadeIn;
    if (fadeIn.durationFrames > 0 && absoluteFrame < start + double(fadeIn.durationFrames))
        return curveBezierValueAt(fadeIn.curve, fadeIn.handles,
            float((absoluteFrame - start) / double(fadeIn.durationFrames)));
    const auto& fadeOut = source_.fadeOut;
    const double outStart = end - double(fadeOut.durationFrames);
    if (fadeOut.durationFrames > 0 && absoluteFrame > outStart)
        return curveBezierValueAt(fadeOut.curve, fadeOut.handles,
            float((absoluteFrame - outStart) / double(fadeOut.durationFrames)));
    return 1.0;
}

}
