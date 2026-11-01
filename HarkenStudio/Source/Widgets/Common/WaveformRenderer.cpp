
#include "WaveformRenderer.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/FontManager.h"

#include <QPainterPath>
#include <QLinearGradient>
#include <algorithm>
#include <cmath>

namespace VNX {

AmplitudeScale WaveformRenderer::amplitudeScale(const WaveformStyle& style, double exponent)
{
    return {kAmplitudeFloorDb, style.amplitudeScale > 0.0
        ? -20.0 * std::log10(style.amplitudeScale) : kAmplitudeFloorDb, exponent};
}


QColor WaveformRenderer::bandToColor(float low, float mid, float high,
                                     const WaveformStyle::MultiBandMode& style)
{
    low  *= style.gains[0];
    mid  *= style.gains[1];
    high *= style.gains[2];

    float maxV = std::max({low, mid, high});
    if (maxV < 1e-6f)
        return {};

    float inv = 1.0f / maxV;
    float wL = low  * inv;
    float wM = mid  * inv;
    float wH = high * inv;

    float r = style.colors[0].redF() * wL
        + style.colors[1].redF() * wM
        + style.colors[2].redF() * wH;
    float g = style.colors[0].greenF() * wL
        + style.colors[1].greenF() * wM
        + style.colors[2].greenF() * wH;
    float b = style.colors[0].blueF() * wL
        + style.colors[1].blueF() * wM
        + style.colors[2].blueF() * wH;

    return QColor::fromRgbF(
        std::clamp(r, 0.0f, 1.0f),
        std::clamp(g, 0.0f, 1.0f),
        std::clamp(b, 0.0f, 1.0f),
        style.alpha * (style.colors[0].alphaF() * wL
            + style.colors[1].alphaF() * wM
            + style.colors[2].alphaF() * wH) / (wL + wM + wH)
    );
}


WaveformRenderer::ResolvedChannel WaveformRenderer::resolveChannel(
    WaveChannelSource source, uint32_t numChannels, bool hasMidSideData)
{
    ResolvedChannel rc;
    switch (source) {
        case WaveChannelSource::Mono:
            rc.ch = -1;
            rc.isMono = true;
            break;
        case WaveChannelSource::Left:
            rc.ch = 0;
            rc.isMono = false;
            break;
        case WaveChannelSource::Right:
            rc.ch = (numChannels > 1) ? 1 : 0;
            rc.isMono = false;
            break;
        case WaveChannelSource::Mid:
            if (hasMidSideData) {
                rc.isMidSide = true;
                rc.msChannel = 0;
            } else {
                rc.ch = -1;
                rc.isMono = true;
            }
            break;
        case WaveChannelSource::Side:
            if (hasMidSideData) {
                rc.isMidSide = true;
                rc.msChannel = 1;
            } else {
                rc.ch = -1;
                rc.isMono = true;
            }
            break;
    }
    return rc;
}


void WaveformRenderer::render(QPainter& painter,
                               const QRect& rect,
                               const WaveformPeakData& data,
                               const WaveformStyle& style,
                               const WaveformRenderOptions& options)
{
    if (!data.isValid() || data.numChannels == 0) return;

    int w = rect.width();
    int h = rect.height();
    if (w <= 0 || h <= 0) return;

    int centerY = rect.y() + h / 2;
    const AmplitudeScale scale = amplitudeScale(style, options.amplitudeExponent);
    const auto offset = [&](double sample) {
        return std::copysign(scale.amplitudeToPosition(std::abs(sample)) * (h / 2), sample);
    };

    auto rc = resolveChannel(options.channel, data.numChannels, data.hasMidSideData());
    if (options.channelIndex >= 0) {
        rc = {};
        rc.ch = std::min(options.channelIndex, static_cast<int>(data.numChannels) - 1);
        rc.isMono = false;
    }
    uint32_t numPeaks = data.numPeaks;
    uint32_t numCh = data.numChannels;
    const double dataFrames = std::round(data.durationSec * data.sampleRate);
    if (options.sampleCurve && data.samplesPerPeak == 1 && dataFrames == numPeaks
        && options.frameCount > 0.0 && options.frameCount < w) {
        const int first = std::max(0, static_cast<int>(std::floor(options.startFrame)));
        const int last = std::min(static_cast<int>(numPeaks) - 1,
            static_cast<int>(std::ceil(options.startFrame + options.frameCount)));
        const uint32_t channel = static_cast<uint32_t>(std::max(0, rc.ch));
        const QColor color = options.accentColor.isValid()
            ? options.accentColor : style.staticMode.peak;
        painter.save();
        painter.setClipRect(rect, Qt::IntersectClip);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 1.25));
        QPainterPath path;
        for (int frame = first; frame <= last; ++frame) {
            float sample = data.peaks[channel * numPeaks * 2 + frame * 2];
            if (rc.isMono && numCh > 1) {
                for (uint32_t ch = 1; ch < numCh; ++ch)
                    sample += data.peaks[ch * numPeaks * 2 + frame * 2];
                sample /= numCh;
            }
            const QPointF point(rect.left() + (frame - options.startFrame)
                                    / options.frameCount * std::max(1, w - 1),
                                centerY - offset(sample));
            if (frame == first)
                path.moveTo(point);
            else
                path.lineTo(point);
            if (options.frameCount * 5 < w) {
                painter.setBrush(color);
                painter.drawEllipse(point, 2.0, 2.0);
            }
        }
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        painter.restore();
        return;
    }
    const auto peakAtFrame = [&](double frame) {
        const double lastStart = double(numPeaks - 1) * data.samplesPerPeak;
        return frame < lastStart
            ? frame / std::max(1u, data.samplesPerPeak)
            : numPeaks - 1 + (frame - lastStart) / std::max(1.0, dataFrames - lastStart);
    };

    bool hasBand = data.hasBandData() && (options.colorMode == WaveColorMode::MultiBand);

    WaveColorMode effectiveMode = options.colorMode;
    if (options.colorMode == WaveColorMode::MultiBand && !data.hasBandData())
        effectiveMode = WaveColorMode::Static;

    for (int x = 0; x < w; ++x) {
        const double peakStartF = options.frameCount > 0.0
            ? peakAtFrame(options.startFrame + double(x) / w * options.frameCount)
            : static_cast<float>(x) / w * numPeaks;
        const double peakEndF = options.frameCount > 0.0
            ? peakAtFrame(options.startFrame + double(x + 1) / w * options.frameCount)
            : static_cast<float>(x + 1) / w * numPeaks;
        int peakStart = std::max(0, static_cast<int>(peakStartF));
        int peakEnd = std::clamp(static_cast<int>(std::ceil(peakEndF)), 1, static_cast<int>(numPeaks));
        if (peakStart >= static_cast<int>(numPeaks)) peakStart = numPeaks - 1;
        if (peakEnd <= peakStart) peakEnd = peakStart + 1;

        float pixelMin, pixelMax;
        float bandLow = 0.0f, bandMid = 0.0f, bandHigh = 0.0f;

        if (rc.isMidSide) {
            uint32_t msBase = rc.msChannel * numPeaks * 2;
            pixelMin =  1.0f;
            pixelMax = -1.0f;

            for (int p = peakStart; p < peakEnd && p < static_cast<int>(numPeaks); ++p) {
                uint32_t off = msBase + p * 2;
                if (data.midSidePeaks[off] < pixelMin) pixelMin = data.midSidePeaks[off];
                if (data.midSidePeaks[off + 1] > pixelMax) pixelMax = data.midSidePeaks[off + 1];
            }
        } else if (rc.isMono && numCh > 1) {
            pixelMin =  1.0f;
            pixelMax = -1.0f;
            float inv = 1.0f / static_cast<float>(numCh);

            for (int p = peakStart; p < peakEnd && p < static_cast<int>(numPeaks); ++p) {
                float sumMin = 0.0f, sumMax = 0.0f;
                for (uint32_t ch = 0; ch < numCh; ++ch) {
                    uint32_t off = ch * numPeaks * 2 + p * 2;
                    sumMin += data.peaks[off];
                    sumMax += data.peaks[off + 1];
                }
                float monoMin = sumMin * inv;
                float monoMax = sumMax * inv;
                if (monoMin < pixelMin) pixelMin = monoMin;
                if (monoMax > pixelMax) pixelMax = monoMax;

                if (hasBand) {
                    for (uint32_t ch = 0; ch < numCh; ++ch) {
                        uint32_t boff = ch * numPeaks * 3 + p * 3;
                        bandLow  += data.bandData[boff]     * inv;
                        bandMid  += data.bandData[boff + 1] * inv;
                        bandHigh += data.bandData[boff + 2] * inv;
                    }
                }
            }

            int bandCount = peakEnd - peakStart;
            if (bandCount > 1 && hasBand) {
                float invB = 1.0f / static_cast<float>(bandCount);
                bandLow *= invB; bandMid *= invB; bandHigh *= invB;
            }
        } else {
            uint32_t ch = static_cast<uint32_t>(std::max(0, rc.ch));
            if (ch >= numCh) ch = 0;

            pixelMin =  1.0f;
            pixelMax = -1.0f;

            for (int p = peakStart; p < peakEnd && p < static_cast<int>(numPeaks); ++p) {
                uint32_t off = ch * numPeaks * 2 + p * 2;
                if (data.peaks[off] < pixelMin) pixelMin = data.peaks[off];
                if (data.peaks[off + 1] > pixelMax) pixelMax = data.peaks[off + 1];

                if (hasBand) {
                    uint32_t boff = ch * numPeaks * 3 + p * 3;
                    bandLow  += data.bandData[boff];
                    bandMid  += data.bandData[boff + 1];
                    bandHigh += data.bandData[boff + 2];
                }
            }

            int bandCount = peakEnd - peakStart;
            if (bandCount > 1 && hasBand) {
                float invB = 1.0f / static_cast<float>(bandCount);
                bandLow *= invB; bandMid *= invB; bandHigh *= invB;
            }
        }

        int yMax = centerY - static_cast<int>(offset(pixelMax));
        int yMin = centerY - static_cast<int>(offset(pixelMin));
        int barH = std::max(1, yMin - yMax);

        QColor col;
        switch (effectiveMode) {
            case WaveColorMode::Static: {
                float amp = std::clamp(std::max(std::abs(pixelMin), std::abs(pixelMax)), 0.0f, 1.0f);
                if (options.accentColor.isValid()) {
                    const auto& sm = style.staticMode;
                    int a = sm.accentQuietAlpha
                        + static_cast<int>(amp * (sm.accentPeakAlpha - sm.accentQuietAlpha));
                    col = options.accentColor;
                    col.setAlphaF(col.alphaF() * a / 255.0);
                } else {
                    const QColor& quiet = style.staticMode.quiet;
                    const QColor& peak = style.staticMode.peak;
                    col = QColor(
                        quiet.red() + static_cast<int>(amp * (peak.red() - quiet.red())),
                        quiet.green() + static_cast<int>(amp * (peak.green() - quiet.green())),
                        quiet.blue() + static_cast<int>(amp * (peak.blue() - quiet.blue())),
                        quiet.alpha() + static_cast<int>(amp * (peak.alpha() - quiet.alpha())));
                }
                break;
            }
            case WaveColorMode::MultiBand:
                col = bandToColor(bandLow, bandMid, bandHigh, style.multiBandMode);
                break;

            case WaveColorMode::ColorMap: {
                float normTop = static_cast<float>(yMax - rect.y()) / static_cast<float>(h);
                float normBot = static_cast<float>(yMin - rect.y()) / static_cast<float>(h);
                float normMid = (normTop + normBot) * 0.5f;
                float distFromCenter = std::abs(normMid - 0.5f) * 2.0f;
                const auto& cm = style.colorMapMode;
                int ri = static_cast<int>(255 * std::clamp(cm.offset[0] + distFromCenter * cm.slope[0], 0.0f, 1.0f));
                int gi = static_cast<int>(255 * std::clamp(cm.offset[1] + distFromCenter * cm.slope[1], 0.0f, 1.0f));
                int bi = static_cast<int>(255 * std::clamp(cm.offset[2] + distFromCenter * cm.slope[2], 0.0f, 1.0f));
                int ai = static_cast<int>(255 * std::clamp(cm.offset[3] + distFromCenter * cm.slope[3], 0.0f, 1.0f));
                col = QColor(ri, gi, bi, ai);
                break;
            }
        }

        painter.fillRect(rect.x() + x, yMax, 1, barH, col);
    }
}


void WaveformRenderer::renderSplitStereo(QPainter& painter,
                                          const QRect& rect,
                                          const WaveformPeakData& data,
                                          const WaveformStyle& style,
                                          WaveColorMode colorMode,
                                          double amplitudeExponent)
{
    if (!data.isValid() || data.numChannels < 2) return;

    int halfH = rect.height() / 2;

    QRect topRect(rect.x(), rect.y(), rect.width(), halfH);
    WaveformRenderOptions options;
    options.colorMode = colorMode;
    options.channel = WaveChannelSource::Left;
    options.amplitudeExponent = amplitudeExponent;
    render(painter, topRect, data, style, options);
    drawDbScale(painter, topRect, style, amplitudeExponent);

    int divY = rect.y() + halfH;
    painter.setPen(QPen(style.splitStereo.divider, style.splitStereo.dividerWidth));
    painter.drawLine(rect.x(), divY, rect.right(), divY);

    QRect botRect(rect.x(), divY, rect.width(), rect.height() - halfH);
    options.channel = WaveChannelSource::Right;
    render(painter, botRect, data, style, options);
    drawDbScale(painter, botRect, style, amplitudeExponent);

    QFont labelFont = FontManager::instance().getFont(
        FontRole::Caption, style.splitStereo.labelPointSize, QFont::Bold);
    painter.setFont(labelFont);
    painter.setPen(style.splitStereo.label);
    painter.drawText(rect.x() + rect.width() - style.splitStereo.labelRightInset,
                     rect.y() + style.splitStereo.labelBaselineOffset,
                     QStringLiteral("L"));
    painter.drawText(rect.x() + rect.width() - style.splitStereo.labelRightInset,
                     divY + style.splitStereo.labelBaselineOffset,
                     QStringLiteral("R"));
}


void WaveformRenderer::renderRawPeaks(QPainter& painter,
                                      const QRectF& rect,
                                      const float* peaks,
                                      int numPeaks,
                                      const WaveformStyle& style,
                                      const QColor& accentColor,
                                      double amplitudeExponent)
{
    if (!peaks || numPeaks <= 0) return;

    const float w = rect.width();
    const float h = rect.height();
    const float midY = rect.y() + h / 2.0f;
    const float halfH = h / 2.0f;
    const AmplitudeScale scale{kAmplitudeFloorDb, 0.0, amplitudeExponent};

    painter.setPen(Qt::NoPen);
    painter.setBrush(style.rawPeaks.background);
    painter.drawRoundedRect(rect, style.rawPeaks.cornerRadius,
                            style.rawPeaks.cornerRadius);

    painter.setPen(Qt::NoPen);
    painter.setBrush(accentColor);

    const float peaksPerPixel = static_cast<float>(numPeaks) / w;
    for (int px = 0; px < static_cast<int>(w); ++px) {
        int peakIdx = static_cast<int>(px * peaksPerPixel);
        if (peakIdx >= numPeaks) peakIdx = numPeaks - 1;

        float minVal = std::clamp(peaks[peakIdx * 2], -1.0f, 1.0f);
        float maxVal = std::clamp(peaks[peakIdx * 2 + 1], -1.0f, 1.0f);

        float y0 = midY - std::copysign(scale.amplitudeToPosition(std::abs(maxVal)) * halfH, maxVal);
        float y1 = midY - std::copysign(scale.amplitudeToPosition(std::abs(minVal)) * halfH, minVal);
        float barH = std::max(1.0f, y1 - y0);

        painter.drawRect(QRectF(rect.x() + px, y0, 1.0f, barH));
    }

    painter.setPen(QPen(style.rawPeaks.centerLine, style.rawPeaks.centerLineWidth));
    painter.drawLine(QPointF(rect.x(), midY), QPointF(rect.x() + w, midY));
}


void WaveformRenderer::drawDbScale(QPainter& painter, const QRect& rect,
                                   const WaveformStyle& style, double amplitudeExponent)
{
    painter.setFont(FontManager::instance().getFont(FontRole::Axis));
    const QFontMetrics metrics(painter.font(), painter.device());
    const int half = rect.height() / 2;
    const auto ticks = buildAmplitudeAxisTicks(rect, rect.y() + half, half, metrics,
        metrics.height() * 2.0, amplitudeScale(style, amplitudeExponent));
    for (const auto& tick : ticks) {
        painter.setPen(QPen(style.dbScale.grid, style.dbScale.gridWidth, Qt::DotLine));
        painter.drawLine(rect.x(), tick.y, rect.right(), tick.y);
        painter.setPen(style.dbScale.label);
        const int top = std::clamp(tick.y - metrics.height() / 2,
            rect.top(), std::max(rect.top(), rect.bottom() - metrics.height() + 1));
        painter.drawText(QRect(rect.x() + style.dbScale.labelLeftInset, top,
            rect.width() - style.dbScale.labelLeftInset, metrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter, tick.label);
    }
}

}
