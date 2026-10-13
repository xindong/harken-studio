#include "VUMeterRenderer.h"
#include "Core/FontManager.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace VNX {
namespace {

constexpr float kPi = 3.14159265358979323846f;

void renderHeader(QPainter& painter,
                  const MeterStyle::VU& style,
                  const VUMeterHeaderFrame& frame)
{
    painter.fillRect(frame.canvasBounds, style.colors.canvasBackground);
    painter.setPen(style.colors.titleText);
    painter.setFont(frame.titleFont);
    painter.drawText(frame.titleBounds,
                     Qt::AlignLeft | Qt::AlignVCenter,
                     frame.title);
}

float positionToAngle(const MeterStyle::VU::Metrics& metrics, qreal position)
{
    const float start = kPi * static_cast<float>(metrics.analogArcStartPiFactor);
    const float end = kPi * static_cast<float>(metrics.analogArcEndPiFactor);
    return start - static_cast<float>(position) * (start - end);
}

QPointF polarPoint(float centerX,
                   float centerY,
                   float radius,
                   float angle)
{
    const float x = centerX + std::cos(angle) * radius;
    const float y = centerY - std::sin(angle) * radius;
    return QPointF(x, y);
}

QPainterPath buildArc(float centerX,
                      float centerY,
                      float radius,
                      float startAngle,
                      float endAngle,
                      int segments)
{
    QPainterPath path;
    for (int index = 0; index <= segments; ++index) {
        const float position = static_cast<float>(index) / segments;
        const float angle = startAngle - position * (startAngle - endAngle);
        const QPointF point = polarPoint(centerX, centerY, radius, angle);
        if (index == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    return path;
}

void renderAnalogDial(QPainter& painter,
                      const MeterStyle::VU& style,
                      const VUAnalogMeterRenderFrame& frame,
                      const VUAnalogDialFrame& dial)
{
    const auto& colors = style.colors;
    const auto& metrics = style.metrics;
    const QRect& area = dial.bounds;

    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.trackBackground);
    painter.drawRoundedRect(area,
                            metrics.analogCornerRadius,
                            metrics.analogCornerRadius);

    const QRect labelStrip(area.left(),
                           area.bottom() - metrics.analogLabelStripBottomOffset,
                           area.width(),
                           metrics.analogLabelStripHeight);
    const QRect inner = area.adjusted(metrics.analogInnerInsetLeft,
                                      metrics.analogInnerInsetTop,
                                      -metrics.analogInnerInsetRight,
                                      -metrics.analogInnerInsetBottom);
    const float centerX = static_cast<float>(inner.center().x());
    const float centerY = static_cast<float>(inner.bottom())
                        - static_cast<float>(metrics.analogPivotBottomOffset);
    const float radius = std::min(inner.width(), inner.height())
                       * static_cast<float>(metrics.analogRadiusScale);
    const float arcStart = positionToAngle(metrics, 0.0);
    const float arcEnd = positionToAngle(metrics, 1.0);

    painter.setPen(QPen(colors.analogArc, metrics.analogArcLineWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(buildArc(centerX,
                              centerY,
                              radius,
                              arcStart,
                              arcEnd,
                              metrics.analogArcSegments));

    painter.setFont(FontManager::instance().getFont(FontRole::Axis));
    const QStringList labels = meterTickLabels(frame.ticks);
    for (int i = 0; i < frame.ticks.size(); ++i) {
        const auto& tick = frame.ticks[i];
        const float angle = positionToAngle(metrics, tick.normalizedPosition);
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        const float innerRadius = radius
                                * static_cast<float>(metrics.analogTickInnerRadiusScale);
        const float outerRadius = radius
                                * static_cast<float>(metrics.analogTickOuterRadiusScale);
        const QColor& tickColor = tick.overload
            ? colors.analogPositiveTick
            : colors.analogNegativeTick;
        painter.setPen(QPen(tickColor, metrics.analogTickLineWidth));
        painter.drawLine(QPointF(centerX + cosine * innerRadius,
                                 centerY - sine * innerRadius),
                         QPointF(centerX + cosine * outerRadius,
                                 centerY - sine * outerRadius));

        const float labelRadius = radius
                                * static_cast<float>(metrics.analogTickLabelRadiusScale);
        painter.setPen(colors.analogNegativeTick);
        painter.drawText(
            QRectF(centerX + cosine * labelRadius
                       - metrics.analogTickLabelHalfWidth,
                   centerY - sine * labelRadius
                       - metrics.analogTickLabelTopOffset,
                   metrics.analogTickLabelWidth,
                   metrics.analogTickLabelHeight),
            Qt::AlignCenter,
            labels[i]);
    }

    const float overloadStart = positionToAngle(
        metrics, frame.overloadStartPosition);
    const float overloadEnd = positionToAngle(
        metrics, frame.overloadEndPosition);
    painter.setPen(QPen(colors.analogOverloadArc,
                        metrics.analogOverloadLineWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(buildArc(
        centerX,
        centerY,
        radius * static_cast<float>(metrics.analogOverloadRadiusScale),
        overloadStart,
        overloadEnd,
        metrics.analogOverloadSegments));

    const float needleAngle = positionToAngle(metrics, dial.needlePosition);
    const float needleLength = radius
                             * static_cast<float>(metrics.analogNeedleLengthScale);
    const QPointF pivot(centerX, centerY);
    const QPointF needleEnd = polarPoint(centerX,
                                         centerY,
                                         needleLength,
                                         needleAngle);
    painter.setPen(QPen(colors.analogNeedleGlow,
                        metrics.analogNeedleGlowWidth));
    painter.drawLine(pivot, needleEnd);
    painter.setPen(QPen(colors.analogNeedle,
                        metrics.analogNeedleWidth));
    painter.drawLine(pivot, needleEnd);

    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.analogPivot);
    painter.drawEllipse(pivot,
                        metrics.analogPivotRadius,
                        metrics.analogPivotRadius);

    painter.setPen(colors.analogValueText);
    painter.setFont(frame.valueFont);
    painter.drawText(QRect(labelStrip.left(),
                           labelStrip.top(),
                           labelStrip.width(),
                           metrics.analogValueHeight),
                     Qt::AlignCenter,
                     dial.valueLabel);
    painter.setPen(colors.analogLabelText);
    painter.setFont(frame.labelFont);
    painter.drawText(QRect(labelStrip.left(),
                           labelStrip.top() + metrics.analogLabelTopOffset,
                           labelStrip.width(),
                           metrics.analogLabelHeight),
                     Qt::AlignCenter,
                     dial.channelLabel);
}

void renderBar(QPainter& painter,
               const MeterStyle& meterStyle,
               const VUBarMeterRenderFrame& frame,
               const VUBarFrame& bar)
{
    const auto& style = meterStyle.vuMeter;
    const auto& colors = style.colors;
    const auto& metrics = style.metrics;
    const QRect& area = bar.bounds;

    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.trackBackground);
    painter.drawRoundedRect(area,
                            metrics.barCornerRadius,
                            metrics.barCornerRadius);

    const QRectF meterRect(area.left() + metrics.barHorizontalInset,
                           bar.levelTop,
                           area.width() - 2.0 * metrics.barHorizontalInset,
                           area.bottom() - bar.levelTop);
    QColor overload = meterStyle.red;
    overload.setAlpha(metrics.barOverloadAlpha);
    QLinearGradient gradient(0, area.top(), 0, area.bottom());
    gradient.setColorAt(metrics.barOverloadStop, overload);
    gradient.setColorAt(metrics.barWarningStop, colors.barWarning);
    gradient.setColorAt(metrics.barYellowStop, meterStyle.yellow);
    gradient.setColorAt(metrics.barBrightGreenStop,
                        meterStyle.green.lighter(metrics.barGreenLighterFactor));
    gradient.setColorAt(metrics.barGreenStop, meterStyle.green);
    painter.fillRect(meterRect, gradient);

    painter.setPen(QPen(colors.barGrid, metrics.barGridLineWidth));
    for (qreal y : bar.gridLines)
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));

    painter.setPen(colors.barLabelText);
    painter.setFont(frame.labelFont);
    painter.drawText(QRect(area.left(),
                           area.bottom() - metrics.barChannelLabelBottomOffset,
                           area.width(),
                           metrics.barChannelLabelHeight),
                     Qt::AlignCenter,
                     bar.channelLabel);

    painter.setPen(colors.barValueText);
    painter.setFont(frame.valueFont);
    painter.drawText(QRect(area.left(),
                           area.bottom() - metrics.barValueLabelBottomOffset,
                           area.width(),
                           metrics.barValueLabelHeight),
                     Qt::AlignCenter,
                     bar.valueLabel);
}

}

VUMeterLayout VUMeterRenderer::layout(const QRect& bounds,
                                      const MeterStyle::VU& style,
                                      VUMeterLayoutMode mode)
{
    const auto& metrics = style.metrics;
    const QRect area = bounds.adjusted(metrics.outerInsetLeft,
                                       metrics.outerInsetTop,
                                       -metrics.outerInsetRight,
                                       -metrics.outerInsetBottom);
    VUMeterLayout result;
    result.titleBounds = QRect(area.left(),
                               area.top(),
                               area.width(),
                               metrics.titleHeight);
    result.contentBounds = area.adjusted(0, metrics.contentTopInset, 0, 0);

    const bool fourItems = mode == VUMeterLayoutMode::AnalogFour
                        || mode == VUMeterLayoutMode::BarFour;
    result.itemCount = fourItems ? 4 : 2;
    if (mode == VUMeterLayoutMode::AnalogTwo
        || mode == VUMeterLayoutMode::AnalogFour) {
        const int gap = metrics.analogGap;
        const int itemWidth = (result.contentBounds.width() - gap) / 2;
        if (fourItems) {
            const int itemHeight = (result.contentBounds.height() - gap) / 2;
            for (int index = 0; index < result.itemCount; ++index) {
                result.itemBounds[index] = QRect(
                    result.contentBounds.left() + (index % 2) * (itemWidth + gap),
                    result.contentBounds.top() + (index / 2) * (itemHeight + gap),
                    itemWidth,
                    itemHeight);
            }
        } else {
            for (int index = 0; index < result.itemCount; ++index) {
                result.itemBounds[index] = QRect(
                    result.contentBounds.left() + index * (itemWidth + gap),
                    result.contentBounds.top(),
                    itemWidth,
                    result.contentBounds.height());
            }
        }
    } else {
        const int gap = metrics.barGap;
        const int itemWidth = (result.contentBounds.width()
                             - gap * (result.itemCount - 1))
                            / result.itemCount;
        for (int index = 0; index < result.itemCount; ++index) {
            result.itemBounds[index] = QRect(
                result.contentBounds.left() + index * (itemWidth + gap),
                result.contentBounds.top(),
                itemWidth,
                result.contentBounds.height());
        }
    }
    return result;
}

void VUMeterRenderer::renderAnalog(QPainter& painter,
                                   const MeterStyle& style,
                                   const VUAnalogMeterRenderFrame& frame)
{
    renderHeader(painter, style.vuMeter, frame.header);
    for (int index = 0; index < frame.dialCount; ++index)
        renderAnalogDial(painter, style.vuMeter, frame, frame.dials[index]);
}

void VUMeterRenderer::renderBars(QPainter& painter,
                                 const MeterStyle& style,
                                 const VUBarMeterRenderFrame& frame)
{
    renderHeader(painter, style.vuMeter, frame.header);
    for (int index = 0; index < frame.barCount; ++index)
        renderBar(painter, style, frame, frame.bars[index]);
}

}
