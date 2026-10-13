
#pragma once

#include "Core/ThemeSnapshot.h"
#include "AmplitudeAxis.h"

#include <QPainter>
#include <QRect>
#include <QColor>
#include <cstdint>

namespace VNX {

struct WaveformPeakData;

enum class WaveColorMode {
    Static,
    MultiBand,
    ColorMap
};

enum class WaveChannelSource {
    Mono,
    Left,
    Right,
    Mid,
    Side
};

struct WaveformFrameRange
{
    uint64_t startFrame = 0;
    uint64_t endFrame = 0;

    bool operator==(const WaveformFrameRange&) const = default;
};

struct WaveformRenderOptions
{
    WaveColorMode colorMode = WaveColorMode::Static;
    WaveChannelSource channel = WaveChannelSource::Mono;
    QColor accentColor;
    int channelIndex = -1;
    double startFrame = 0.0;
    double frameCount = 0.0;
    bool sampleCurve = false;
    double amplitudeExponent = 1.0;
};

class WaveformRenderer
{
public:
    static void render(QPainter& painter,
                       const QRect& rect,
                       const WaveformPeakData& data,
                       const WaveformStyle& style,
                       const WaveformRenderOptions& options = {});

    static void renderSplitStereo(QPainter& painter,
                                  const QRect& rect,
                                  const WaveformPeakData& data,
                                  const WaveformStyle& style,
                                  WaveColorMode colorMode = WaveColorMode::Static,
                                  double amplitudeExponent = 1.0);

    static void renderRawPeaks(QPainter& painter,
                               const QRectF& rect,
                               const float* peaks,
                               int numPeaks,
                               const WaveformStyle& style,
                               const QColor& accentColor,
                               double amplitudeExponent = 1.0);

    static void drawDbScale(QPainter& painter, const QRect& rect,
                            const WaveformStyle& style, double amplitudeExponent = 1.0);

    static AmplitudeScale amplitudeScale(const WaveformStyle& style,
                                         double exponent = 1.0);

    static QColor bandToColor(float low, float mid, float high,
                              const WaveformStyle::MultiBandMode& style);

private:
    struct ResolvedChannel {
        int ch = 0;
        bool isMono = true;
        bool isMidSide = false;
        int msChannel = 0;
    };
    static ResolvedChannel resolveChannel(WaveChannelSource source,
                                           uint32_t numChannels,
                                           bool hasMidSideData);
};

}
