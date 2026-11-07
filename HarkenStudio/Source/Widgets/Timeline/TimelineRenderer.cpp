#include "TimelineRenderer.h"

#include "Core/FontManager.h"
#include "Core/NumericFormat.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/WaveformRenderer.h"

#include <QBrush>
#include <QFontMetrics>
#include <QHash>
#include <QPainter>
#include <QPen>
#include <QPolygon>
#include <QRegion>

#include <algorithm>
#include <cmath>

namespace VNX {
namespace {

QColor withOpacity(QColor color, qreal opacity)
{
    color.setAlphaF(color.alphaF() * opacity);
    return color;
}

struct CursorStroke
{
    qreal center;
    qreal width;
};

CursorStroke cursorStroke(const QPainter& painter, int x)
{
    const QTransform transform = painter.deviceTransform();
    const qreal scale = transform.m11();
    const int width = std::max(1, int(std::lround(scale)));
    const qreal position = transform.map(QPointF(x + 0.5, 0)).x();
    const qreal center = width % 2 ? std::floor(position) + 0.5 : std::round(position);
    return {(center - transform.dx()) / scale, width / scale};
}

QColor clipFill(const TimelineClipItem& item, const TimelineStyleView& style)
{
    const QColor base = item.objectColor.isValid() ? item.objectColor : style.timeline.clip;
    switch (item.state) {
    case TimelineClipVisualState::Hover:
        return withOpacity(base, 61 / 255.0);
    case TimelineClipVisualState::Selected:
        return withOpacity(base, 71 / 255.0);
    case TimelineClipVisualState::Normal:
        break;
    }
    return withOpacity(base, 41 / 255.0);
}

QColor clipTitleFill(const TimelineClipItem& item, const TimelineStyleView& style)
{
    const bool selected = item.state == TimelineClipVisualState::Selected;
    const QColor base = item.objectColor.isValid() ? item.objectColor
        : selected ? style.timeline.clipSelected : style.timeline.clip;
    return withOpacity(base, (selected ? 150 : item.state == TimelineClipVisualState::Hover ? 115 : 90) / 255.0);
}

void drawViewport(QPainter& painter,
                  const TimelineStyleView& style,
                  const TimelineRenderData& data)
{
    if (data.viewport.isEmpty())
        return;
    painter.fillRect(data.viewport, style.canvasBackground);
}

void drawLaneRects(QPainter& painter,
                   const TimelineStyleView& style,
                   const TimelineRenderData& data)
{
    for (const TimelineLaneRect& lane : data.laneRects) {
        QColor fill = lane.alternate ? style.trackBackgroundAlt
                                     : style.trackBackground;
        if (lane.selected)
            fill = style.metrics.timeline.laneHeight > 0
                ? (style.canvasBackground.isValid() ? fill.lighter(108) : fill)
                : fill;
        if (lane.highlighted)
            fill = fill.lighter(118);
        painter.fillRect(lane.rect, fill);
        if (lane.selected)
            painter.fillRect(lane.rect, withOpacity(style.accent.isValid()
                                                      ? style.accent
                                                      : fill, 28 / 255.0));
        if (lane.accent.isValid())
            painter.fillRect(QRect(lane.rect.left(), lane.rect.top(),
                                   3, lane.rect.height()),
                             lane.accent);
    }
}

void drawGrid(QPainter& painter,
              const TimelineStyleView& style,
              const TimelineRenderData& data)
{
    if (!data.timeAxis.ticks.isEmpty()) {
        const QColor& uniform = style.uniformTimeAxisGrid;
        drawTimeAxisGrid(painter, uniform.isValid() ? uniform : style.curve.colors.grid,
            data.viewport, data.timeAxis,
            uniform.isValid() ? uniform : withOpacity(style.curve.colors.grid, 0.5),
            uniform.isValid() ? uniform : withOpacity(style.curve.colors.grid, 0.75));
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(style.curve.colors.grid);
        for (const auto& tick : data.amplitudeTicks)
            painter.drawLine(data.viewport.left(), tick.y, data.viewport.right(), tick.y);
        painter.restore();
        return;
    }
    for (const TimelineGridLine& line : data.gridLines) {
        painter.setPen(line.major ? style.music.beatGridMajor
                                  : style.music.beatGridMinor);
        painter.drawLine(line.line);
    }
    painter.setPen(style.curve.colors.grid);
    for (const auto& tick : data.amplitudeTicks)
        painter.drawLine(data.viewport.left(), tick.y, data.viewport.right(), tick.y);
}

void drawSemanticBands(QPainter& painter,
                       const TimelineStyleView& style,
                       const TimelineRenderData& data)
{
    const auto& timeline = style.timeline;
    const QColor veil = timeline.outOfSpanVeil;
    const QColor veilLabel = style.textMuted.isValid()
        ? style.textMuted
        : timeline.unavailableText;
    for (const TimelineSemanticBand& band : data.semanticBands) {
        switch (band.kind) {
        case TimelineSemanticBandKind::Nominal:
            painter.fillRect(band.rect, withOpacity(timeline.clipSelected, 24 / 255.0));
            break;
        case TimelineSemanticBandKind::Pickup:
        case TimelineSemanticBandKind::Tail:
            painter.fillRect(band.rect, veil);
            break;
        case TimelineSemanticBandKind::Unavailable:
            painter.fillRect(band.rect,
                             QBrush(timeline.unavailableOverlay, Qt::BDiagPattern));
            break;
        }
        if (band.label.isEmpty() || band.rect.width() < 40)
            continue;
        painter.setPen(veilLabel);
        painter.setFont(FontManager::instance().getFont(FontRole::Caption));
        painter.drawText(band.rect.adjusted(0, 1, 0, 0),
                         Qt::AlignHCenter | Qt::AlignTop, band.label);
    }
}

void drawWaveform(QPainter& painter,
                  const TimelineClipItem& item,
                  const TimelineStyleView& style)
{
    if (!item.waveform || !item.waveform->isValid() || item.rect.width() < 8
        || item.waveformRect.isEmpty())
        return;
    painter.save();
    painter.setClipRect(item.waveformViewport.intersected(item.rect), Qt::IntersectClip);
    WaveformRenderOptions options;
    options.sourceView = item.waveformView;
    options.startFrame = item.waveformStartFrame;
    options.frameCount = item.waveformFrameCount;
    options.sampleCurve = true;
    options.amplitudeExponent = style.waveformExponent;
    if (item.objectColor.isValid())
        options.accentColor = item.objectColor;
    WaveformRenderer::render(painter, item.waveformRect, *item.waveform, style.waveform, options);
    painter.restore();
}

void drawClipChrome(QPainter& painter,
                    const TimelineClipItem& item,
                    const TimelineStyleView& style,
                    const QRect& title,
                    const QFontMetrics& metrics)
{
    if (title.isEmpty())
        return;
    painter.save();
    painter.setClipRect(title, Qt::IntersectClip);
    const int gap = style.metrics.spacingXSmall;
    int left = title.left() + gap;
    const int icon = std::min(16, title.height());
    if (!item.typeIcon.isNull() && title.width() >= icon + gap * 2) {
        const QRect iconRect(left, title.top() + (title.height() - icon) / 2, icon, icon);
        item.typeIcon.paint(&painter, iconRect);
        left = iconRect.right() + gap;
    }

    const int right = title.right() - gap;

    if (left >= right || item.label.isEmpty()) {
        painter.restore();
        return;
    }
    painter.setPen(style.timeline.clipText);
    const QString elided = metrics.elidedText(item.label, Qt::ElideRight,
                                              std::max(0, right - left));
    painter.drawText(QRect(left, title.top(), right - left, title.height()),
                     Qt::AlignVCenter | Qt::AlignLeft, elided);
    painter.restore();
}

void drawEmergence(QPainter& painter,
                   const TimelineClipItem& item,
                   const TimelineStyleView& style,
                   int titleHeight)
{
    if (!item.emergencePixel)
        return;
    const QRect body = item.rect.adjusted(0, titleHeight, 0, 0);
    if (body.isEmpty())
        return;
    const int x = std::clamp(*item.emergencePixel,
                             item.rect.left() + 3,
                             item.rect.right() - 3);
    if (item.emergenceClamped) {
        painter.setPen(QPen(style.timeline.emergenceMarker, 1, Qt::DotLine));
        painter.drawLine(body.left() + 2, body.top() + 2,
                         body.right() - 2, body.top() + 2);
    }
    painter.setPen(QPen(withOpacity(style.timeline.emergenceMarker, 70 / 255.0), 6));
    painter.drawLine(x, body.top(), x, body.bottom());
    painter.setPen(QPen(style.timeline.emergenceMarker, 2));
    painter.drawLine(x, body.top(), x, body.bottom());
}

void drawClipItem(QPainter& painter,
                  const TimelineStyleView& style,
                  const TimelineClipItem& item,
                  const QRect& labelRect,
                  const QFontMetrics& metrics,
                  int titleHeight)
{
    if (item.rect.isEmpty())
        return;
    painter.save();
    painter.setClipRect(item.rect, Qt::IntersectClip);
    if (item.residual)
        painter.setOpacity(0.35);
    else if (item.muted)
        painter.setOpacity(0.3);
    painter.fillRect(item.rect, clipFill(item, style));
    drawWaveform(painter, item, style);
    drawEmergence(painter, item, style, titleHeight);
    const QRect title(item.rect.topLeft(), QSize(item.rect.width(),
        std::min(item.rect.height(), titleHeight)));
    painter.fillRect(title, style.canvasBackground);
    painter.fillRect(title, clipTitleFill(item, style));
    drawClipChrome(painter, item, style, labelRect, metrics);
    painter.restore();
}

void drawClips(QPainter& painter,
               const TimelineStyleView& style,
               const TimelineRenderData& data)
{
    if (data.clipItems.isEmpty())
        return;
    const QFont font = FontManager::instance().getFont(FontRole::Caption);
    const QFontMetrics metrics(font);
    QVector<QRect> labels(data.clipItems.size());
    QHash<int, QRegion> coveredRows;
    const int titleHeight = metrics.height() + style.metrics.table.cellPaddingY * 2;
    for (qsizetype i = data.clipItems.size(); i-- > 0;) {
        const auto& rect = data.clipItems[i].rect;
        const QRect title(rect.topLeft(), QSize(rect.width(), std::min(rect.height(), titleHeight)));
        const QRect visibleTitle = data.viewport.isEmpty() ? title : title.intersected(data.viewport);
        QRegion visible(visibleTitle);
        QRegion& covered = coveredRows[title.top()];
        visible -= covered;
        for (const QRect& part : visible)
            if (part.topLeft() == visibleTitle.topLeft()) {
                labels[i] = part;
                break;
            }
        covered += title;
    }
    painter.save();
    painter.setFont(font);
    for (qsizetype i = 0; i < data.clipItems.size(); ++i)
        drawClipItem(painter, style, data.clipItems[i], labels[i], metrics, titleHeight);
    painter.restore();
}

void drawCrossfades(QPainter& painter,
                    const TimelineStyleView& style,
                    const TimelineRenderData& data)
{
    const QColor fill = withOpacity(style.timeline.transition, 26 / 255.0);
    const QColor line = withOpacity(style.timeline.transition, 220 / 255.0);
    for (const TimelineCrossfadeRegion& region : data.crossfadeRegions) {
        if (region.rect.isEmpty())
            continue;
        painter.fillRect(region.rect, fill);
        painter.setPen(QPen(line, 1));
        painter.drawLine(region.rect.topLeft(), region.rect.bottomRight());
        painter.drawLine(region.rect.topRight(), region.rect.bottomLeft());
    }
}

QColor markerColor(const TimelineStyleView& style, const TimelineMarker& marker)
{
    if (marker.color.isValid())
        return marker.color;
    switch (marker.kind) {
    case MarkerKind::Entry:
        return style.timeline.entry;
    case MarkerKind::Exit:
        return style.timeline.exit;
    case MarkerKind::Custom:
        return style.timeline.transition;
    }
    return style.timeline.transition;
}

void drawMarkerHandle(QPainter& painter, const TimelineMarker& marker, const QColor& color,
                      const QColor& grooveColor, const ThemeMetrics& metrics)
{
    const int x = marker.x;
    const int h = std::min(marker.height, metrics.timeAxis.handleBandHeight);
    QPolygon flag;
    switch (marker.shape) {
    case TimelineMarkerShape::EntryHandle:
    case TimelineMarkerShape::ExitHandle:
        drawTimeAxisHandle(painter, color, grooveColor,
            TimelineRenderer::markerHandleRect(x, marker.height, marker.shape, metrics.timeAxis));
        return;
    case TimelineMarkerShape::Diamond:
        flag << QPoint(x, 1) << QPoint(x + metrics.timeAxis.handleWidth / 2, h / 2)
             << QPoint(x, h - 1) << QPoint(x - metrics.timeAxis.handleWidth / 2, h / 2);
        break;
    case TimelineMarkerShape::Line:
        break;
    }
    if (flag.isEmpty())
        return;
    if (marker.selected) {
        painter.setPen(QPen(withOpacity(color, 80 / 255.0), 6));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(flag);
        painter.setPen(QPen(color, 2));
        painter.setBrush(color);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(withOpacity(color, 220 / 255.0));
    }
    painter.drawPolygon(flag);
}

void drawMarker(QPainter& painter, const TimelineMarker& marker, const QColor& color,
                const QColor& grooveColor, const ThemeMetrics& metrics)
{
    painter.save();
    const QColor handleColor = marker.selected || marker.hovered ? color.lighter(130) : color;
    painter.setRenderHint(QPainter::Antialiasing, false);
    if (marker.kind == MarkerKind::Entry || marker.kind == MarkerKind::Exit) {
        painter.setPen(QPen(handleColor, marker.selected || marker.hovered ? 2 : 1));
        painter.drawLine(marker.x, 0, marker.x, marker.height - 1);
    } else {
        const int lineAlpha = marker.shape == TimelineMarkerShape::Line
            ? (marker.selected ? 160 : 90)
            : (marker.selected ? 255 : 180);
        painter.setPen(QPen(withOpacity(color, lineAlpha / 255.0), 2));
        painter.drawLine(marker.x, 1, marker.x, marker.height - 2);
    }
    painter.setRenderHint(QPainter::Antialiasing, marker.shape == TimelineMarkerShape::Diamond);
    drawMarkerHandle(painter, marker, handleColor, grooveColor, metrics);
    const int labelWidth = marker.labelMaxWidth;
    if (!marker.label.isEmpty() && labelWidth >= metrics.timeAxis.handleWidth * 2) {
        painter.setFont(FontManager::instance().getFont(FontRole::Caption));
        painter.setPen(color);
        const QFontMetrics fontMetrics(painter.font());
        const QString elided = fontMetrics.elidedText(marker.label, Qt::ElideRight, labelWidth);
        const bool leftLabel = marker.shape == TimelineMarkerShape::ExitHandle;
        const int inset = metrics.timeAxis.handleWidth + metrics.timeAxis.labelPadding;
        const int labelX = leftLabel ? marker.x - labelWidth - inset : marker.x + inset;
        painter.drawText(QRect(labelX, 0, labelWidth, marker.height),
                         Qt::AlignVCenter | (leftLabel ? Qt::AlignRight : Qt::AlignLeft), elided);
    }
    painter.restore();
}

void drawMarkers(QPainter& painter,
              const TimelineStyleView& style,
              const TimelineRenderData& data)
{
    for (const TimelineMarker& marker : data.markers)
        drawMarker(painter, marker, markerColor(style, marker), style.canvasBackground, style.metrics);
}

void drawPlaceholder(QPainter& painter,
                     const TimelineStyleView& style,
                     const TimelineRenderData& data)
{
    if (data.placeholder.isEmpty())
        return;
    painter.setPen(style.placeholderText);
    painter.drawText(data.viewport, Qt::AlignCenter, data.placeholder);
}

} // namespace

void TimelineRenderer::drawVerticalLine(QPainter& painter, const QColor& color, int x, const QRect& bounds)
{
    if (!color.isValid() || bounds.isEmpty())
        return;
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(color, 1));
    painter.drawLine(x, bounds.top(), x, bounds.bottom() + 1);
    painter.restore();
}

void TimelineRenderer::drawCursorLine(QPainter& painter, const QColor& color, int x, const QRect& bounds)
{
    if (!color.isValid() || bounds.isEmpty())
        return;
    const auto stroke = cursorStroke(painter, x);
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(QRectF(stroke.center - stroke.width / 2, bounds.top(),
        stroke.width, bounds.height()), color);
    painter.restore();
}

void TimelineRenderer::drawCursorHandle(QPainter& painter, const QColor& color, const QRect& bounds)
{
    if (!color.isValid() || bounds.isEmpty())
        return;
    const auto stroke = cursorStroke(painter, bounds.center().x());
    const QTransform transform = painter.deviceTransform();
    const qreal halfWidth = (std::round(bounds.width() * transform.m11()) - 1) / (2 * transform.m11());
    const qreal top = (std::round(transform.map(QPointF(0, bounds.top())).y())
        + 0.5 - transform.dy()) / transform.m22();
    const qreal bottom = (std::round(transform.map(QPointF(0, bounds.bottom() + 1)).y())
        - 0.5 - transform.dy()) / transform.m22();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color.lighter(145), 0));
    painter.setBrush(color);
    painter.drawPolygon(QPolygonF{QPointF(stroke.center - halfWidth, top),
        QPointF(stroke.center + halfWidth, top), QPointF(stroke.center, bottom)});
    painter.restore();
}

int TimelineRenderer::clipTitleHeight(const ThemeMetrics& metrics)
{
    return QFontMetrics(FontManager::instance().getFont(FontRole::Caption)).height()
        + metrics.table.cellPaddingY * 2;
}

QRect TimelineRenderer::markerHandleRect(int x, int height, TimelineMarkerShape shape,
                                          const ThemeMetrics::TimeAxis& geometry)
{
    if (shape != TimelineMarkerShape::EntryHandle && shape != TimelineMarkerShape::ExitHandle)
        return {};
    QRect bounds = timeAxisHandleRect(x, 0, shape == TimelineMarkerShape::EntryHandle, geometry);
    bounds.setHeight(std::min(height, bounds.height()));
    return bounds;
}

void TimelineRenderer::render(QPainter& painter,
                              const TimelineStyleView& style,
                              const TimelineRenderData& data)
{
    if (!data.viewport.isEmpty()) {
        drawViewport(painter, style, data);
        if (!data.frameless)
            drawLaneRects(painter, style, data);
        drawGrid(painter, style, data);
        drawSemanticBands(painter, style, data);
        if (!data.frameless)
            for (const TimelineLaneRect& lane : data.laneRects)
                painter.fillRect(QRect(lane.rect.left(), lane.rect.bottom(), lane.rect.width(), 1),
                    style.canvasBorder);
        if (data.clipItems.isEmpty() && !data.placeholder.isEmpty())
            drawPlaceholder(painter, style, data);
        else {
            drawClips(painter, style, data);
            drawCrossfades(painter, style, data);
        }
    } else {
        drawClips(painter, style, data);
        drawCrossfades(painter, style, data);
    }
    if (data.bottomBorder && !data.viewport.isEmpty())
        painter.fillRect(QRect(data.viewport.left(), data.viewport.bottom(), data.viewport.width(), 1),
            style.canvasBorder);
    drawMarkers(painter, style, data);
    if (data.nominalBar.visible)
        painter.fillRect(data.nominalBar.rect, style.timeline.entry);
    if (data.editCursor.visible)
        drawCursorLine(painter, data.editCursor.highlighted ? style.music.cursor.lighter(130)
            : style.music.cursor, data.editCursor.x, data.viewport);
    if (data.playhead.visible)
        drawCursorLine(painter, style.music.cursor, data.playhead.x, data.viewport);
    if (data.dropIndicator.visible)
        drawVerticalLine(painter, style.canvasBorder, data.dropIndicator.x, data.viewport);
    if (data.snapGuide.visible) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(withOpacity(style.accent, 160 / 255.0), 1));
        painter.drawLine(data.snapGuide.x, data.viewport.top(),
                         data.snapGuide.x, data.viewport.bottom());
        painter.restore();
    }
    if (data.lasso.visible) {
        painter.setPen(QPen(style.canvasBorder, 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(data.lasso.rect.normalized());
    }
}

QVector<AmplitudeAxisTick> TimelineRenderer::amplitudeTicks(
    const QRect& viewport, double zoom, int scroll, const ThemeSnapshot& theme,
    double amplitudeExponent)
{
    const QFontMetrics metrics(FontManager::instance().getFont(FontRole::Caption));
    const int height = int(std::lround(viewport.height() * zoom));
    const QRect wave = waveformAmplitudeBounds(viewport, height, scroll, metrics, theme.ui.metrics.timeAxis.labelPadding);
    const int center = wave.top() + wave.height() / 2;
    auto ticks = buildAmplitudeAxisTicks(viewport, center, wave.height() / 2, metrics,
        theme.domains.curve.metrics.amplitudeGridTargetRowSpacing,
        WaveformRenderer::amplitudeScale(theme.domains.waveform, amplitudeExponent));
    for (auto& tick : ticks) {
        const QRect label(viewport.left(), tick.y - metrics.height() / 2, viewport.width(), metrics.height());
        if (!tick.major && !viewport.contains(label))
            tick.label.clear();
    }
    return ticks;
}

void TimelineRenderer::drawTickSet(QPainter& painter,
                                   const TimelineStyleView& style,
                                   const TimelineTickSet& ticks,
                                   int originX,
                                   int height,
                                   int leftBound,
                                   int rightBound,
                                   int labelTop,
                                   int labelHeight)
{
    const auto& metrics = style.metrics.timeline;
    painter.save();
    painter.setFont(FontManager::instance().getFont(FontRole::Caption));
    for (const TimelineTick& tick : ticks.ticks) {
        const int x = originX + tick.pixel;
        if (x < leftBound || x >= rightBound)
            continue;
        painter.setPen(tick.major ? style.music.beatGridMajor
                                  : style.music.beatGridMinor);
        const int length = tick.major ? metrics.majorTickLength : metrics.tickLength;
        painter.drawLine(x, height - length, x, height);
        if (!tick.labeled)
            continue;
        painter.setPen(style.axisText);
        painter.drawText(QRect(x + style.metrics.timeAxis.labelPadding, labelTop,
                              painter.fontMetrics().horizontalAdvance(tick.label), labelHeight),
                         Qt::AlignVCenter | Qt::AlignLeft, tick.label);
    }
    painter.restore();
}

void TimelineRenderer::drawMarker(QPainter& painter, int x, int height,
                               const QString& label, const QColor& color)
{
    TimelineRenderData data;
    TimelineMarker marker;
    marker.x = x;
    marker.height = height;
    marker.label = label;
    marker.color = color;
    data.markers.append(marker);
    render(painter, TimelineStyleView{}, data);
}

void TimelineRenderer::drawClip(QPainter& painter, const QRect& area,
                                const decltype(DomainColors::timeline)& clipStyle,
                                bool selected, const QString& label)
{
    TimelineRenderData data;
    TimelineClipItem item;
    item.rect = area;
    item.label = label;
    item.state = selected ? TimelineClipVisualState::Selected
                          : TimelineClipVisualState::Normal;
    data.clipItems.append(item);
    TimelineStyleView style;
    style.timeline = clipStyle;
    style.metrics = ThemeManager::instance().metrics();
    render(painter, style, data);
}

}
